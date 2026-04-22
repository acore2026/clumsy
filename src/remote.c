#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <Windows.h>
#include "common.h"

#define REMOTE_REQUEST_MAX 4096
#define REMOTE_RESPONSE_MAX 8192
#define REMOTE_WAIT_TIMEOUT_MS 10000

typedef enum {
    REMOTE_COMMAND_NONE = 0,
    REMOTE_COMMAND_ARM,
    REMOTE_COMMAND_DISARM,
} RemoteCommandType;

typedef struct {
    RemoteCommandType type;
    char scenarioName[SCENARIO_NAME_SIZE];
    int resultCode;
    char message[MSG_BUFSIZE];
    BOOL pending;
} RemoteCommand;

typedef struct {
    BOOL filtering;
    char activeScenario[SCENARIO_NAME_SIZE];
} RemoteRuntimeState;

static HANDLE remoteThread = NULL;
static SOCKET listenSocket = INVALID_SOCKET;
static HWND remoteWindow = NULL;
static volatile LONG stopRequested = 0;
static CRITICAL_SECTION stateLock;
static CRITICAL_SECTION commandLock;
static HANDLE commandDoneEvent = NULL;
static HANDLE serverStartupEvent = NULL;
static BOOL locksInitialized = FALSE;
static BOOL wsaStarted = FALSE;
static volatile LONG remoteRunning = 0;
static volatile LONG remoteStartSucceeded = 0;
static RemoteCommand pendingCommand = {0};
static RemoteRuntimeState runtimeState = {0};

static DWORD WINAPI remoteServerThreadMain(LPVOID arg);

static const char* reasonPhrase(int statusCode) {
    switch (statusCode) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
}

static void ensureRemoteStateStorage() {
    if (!locksInitialized) {
        InitializeCriticalSection(&stateLock);
        InitializeCriticalSection(&commandLock);
        commandDoneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        serverStartupEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        locksInitialized = TRUE;
    }
}

static void closeSocketSafe(SOCKET *sock) {
    if (*sock != INVALID_SOCKET) {
        closesocket(*sock);
        *sock = INVALID_SOCKET;
    }
}

static BOOL sendAll(SOCKET sock, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int rc = send(sock, buf + sent, len - sent, 0);
        if (rc <= 0) {
            return FALSE;
        }
        sent += rc;
    }
    return TRUE;
}

static int appendJsonEscaped(char *dst, int offset, int maxLen, const char *src) {
    while (*src && offset < maxLen - 1) {
        unsigned char ch = (unsigned char)*src++;
        if (ch == '\\' || ch == '"') {
            if (offset + 2 >= maxLen) {
                break;
            }
            dst[offset++] = '\\';
            dst[offset++] = (char)ch;
        } else if (ch == '\r') {
            if (offset + 2 >= maxLen) {
                break;
            }
            dst[offset++] = '\\';
            dst[offset++] = 'r';
        } else if (ch == '\n') {
            if (offset + 2 >= maxLen) {
                break;
            }
            dst[offset++] = '\\';
            dst[offset++] = 'n';
        } else if (ch >= 32) {
            dst[offset++] = (char)ch;
        }
    }
    dst[offset] = '\0';
    return offset;
}

static void sendJsonResponse(SOCKET sock, int statusCode, const char *jsonBody) {
    char response[REMOTE_RESPONSE_MAX];
    int bodyLen = (int)strlen(jsonBody);
    int headerLen = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        statusCode,
        reasonPhrase(statusCode),
        bodyLen
    );

    if (headerLen < 0) {
        return;
    }

    sendAll(sock, response, headerLen);
    sendAll(sock, jsonBody, bodyLen);
}

static void buildErrorJson(char out[REMOTE_RESPONSE_MAX], const char *message) {
    int offset = snprintf(out, REMOTE_RESPONSE_MAX, "{\"ok\":false,\"error\":\"");
    offset = appendJsonEscaped(out, offset, REMOTE_RESPONSE_MAX, message ? message : "Unknown error");
    snprintf(out + offset, REMOTE_RESPONSE_MAX - offset, "\"}");
}

static BOOL decodeUrlSegment(const char *encoded, char decoded[SCENARIO_NAME_SIZE]) {
    int outIx = 0;
    while (*encoded && outIx < SCENARIO_NAME_SIZE - 1) {
        if (*encoded == '%') {
            char hex[3];
            char *endPtr;
            long value;
            if (encoded[1] == '\0' || encoded[2] == '\0') {
                return FALSE;
            }
            hex[0] = encoded[1];
            hex[1] = encoded[2];
            hex[2] = '\0';
            value = strtol(hex, &endPtr, 16);
            if (*endPtr != '\0') {
                return FALSE;
            }
            decoded[outIx++] = (char)value;
            encoded += 3;
        } else if (*encoded == '+') {
            decoded[outIx++] = ' ';
            ++encoded;
        } else {
            decoded[outIx++] = *encoded++;
        }
    }
    decoded[outIx] = '\0';
    return *encoded == '\0';
}

static BOOL receiveHttpRequest(SOCKET client, char request[REMOTE_REQUEST_MAX]) {
    int total = 0;

    memset(request, 0, REMOTE_REQUEST_MAX);
    for (;;) {
        int rc = recv(client, request + total, REMOTE_REQUEST_MAX - total - 1, 0);
        if (rc <= 0) {
            return FALSE;
        }
        total += rc;
        request[total] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL || total >= REMOTE_REQUEST_MAX - 1) {
            return TRUE;
        }
    }
}

static void snapshotRuntimeState(RemoteRuntimeState *state) {
    if (!locksInitialized) {
        memset(state, 0, sizeof(*state));
        return;
    }
    EnterCriticalSection(&stateLock);
    *state = runtimeState;
    LeaveCriticalSection(&stateLock);
}

void remoteServerSetRuntimeState(BOOL filtering, const char *activeScenario) {
    if (!locksInitialized) {
        return;
    }
    EnterCriticalSection(&stateLock);
    runtimeState.filtering = filtering;
    runtimeState.activeScenario[0] = '\0';
    if (activeScenario != NULL) {
        strncpy(runtimeState.activeScenario, activeScenario, sizeof(runtimeState.activeScenario) - 1);
    }
    LeaveCriticalSection(&stateLock);
}

static BOOL submitUiCommand(RemoteCommandType type, const char *scenarioName, int *statusCode, char message[MSG_BUFSIZE]) {
    BOOL posted;

    if (!locksInitialized || remoteWindow == NULL || commandDoneEvent == NULL) {
        strcpy(message, "Remote control is not ready.");
        *statusCode = 503;
        return FALSE;
    }

    EnterCriticalSection(&commandLock);
    if (pendingCommand.pending) {
        LeaveCriticalSection(&commandLock);
        strcpy(message, "Another remote command is already in progress.");
        *statusCode = 503;
        return FALSE;
    }
    memset(&pendingCommand, 0, sizeof(pendingCommand));
    pendingCommand.type = type;
    pendingCommand.pending = TRUE;
    if (scenarioName != NULL) {
        strncpy(pendingCommand.scenarioName, scenarioName, sizeof(pendingCommand.scenarioName) - 1);
    }
    ResetEvent(commandDoneEvent);
    LeaveCriticalSection(&commandLock);

    posted = PostMessage(remoteWindow, WM_CLUMSY_REMOTE_COMMAND, 0, 0);
    if (!posted) {
        EnterCriticalSection(&commandLock);
        pendingCommand.pending = FALSE;
        LeaveCriticalSection(&commandLock);
        strcpy(message, "Failed to post remote command to UI thread.");
        *statusCode = 500;
        return FALSE;
    }

    if (WaitForSingleObject(commandDoneEvent, REMOTE_WAIT_TIMEOUT_MS) != WAIT_OBJECT_0) {
        EnterCriticalSection(&commandLock);
        pendingCommand.pending = FALSE;
        LeaveCriticalSection(&commandLock);
        strcpy(message, "Timed out waiting for UI thread to process command.");
        *statusCode = 503;
        return FALSE;
    }

    EnterCriticalSection(&commandLock);
    *statusCode = pendingCommand.resultCode;
    strncpy(message, pendingCommand.message, MSG_BUFSIZE - 1);
    pendingCommand.pending = FALSE;
    LeaveCriticalSection(&commandLock);
    return TRUE;
}

BOOL remoteServerDispatchCommand() {
    RemoteCommandType type;
    char scenarioName[SCENARIO_NAME_SIZE];
    char message[MSG_BUFSIZE] = {0};
    int resultCode = 500;

    if (!locksInitialized) {
        return FALSE;
    }

    EnterCriticalSection(&commandLock);
    if (!pendingCommand.pending) {
        LeaveCriticalSection(&commandLock);
        return FALSE;
    }
    type = pendingCommand.type;
    strncpy(scenarioName, pendingCommand.scenarioName, sizeof(scenarioName) - 1);
    scenarioName[sizeof(scenarioName) - 1] = '\0';
    LeaveCriticalSection(&commandLock);

    switch (type) {
    case REMOTE_COMMAND_ARM:
        resultCode = armScenarioByName(scenarioName, message, MSG_BUFSIZE);
        break;
    case REMOTE_COMMAND_DISARM:
        resultCode = disarmActiveScenario(message, MSG_BUFSIZE);
        break;
    default:
        strcpy(message, "Unknown remote command.");
        resultCode = 500;
        break;
    }

    EnterCriticalSection(&commandLock);
    pendingCommand.resultCode = resultCode;
    strncpy(pendingCommand.message, message, sizeof(pendingCommand.message) - 1);
    LeaveCriticalSection(&commandLock);
    SetEvent(commandDoneEvent);
    return TRUE;
}

static void handleHealth(SOCKET client) {
    RemoteRuntimeState state;
    char body[REMOTE_RESPONSE_MAX];
    snapshotRuntimeState(&state);

    if (state.activeScenario[0] != '\0') {
        int offset = snprintf(body, sizeof(body), "{\"server\":\"running\",\"filtering\":%s,\"activeScenario\":\"",
            state.filtering ? "true" : "false");
        offset = appendJsonEscaped(body, offset, sizeof(body), state.activeScenario);
        snprintf(body + offset, sizeof(body) - offset, "\"}");
    } else {
        snprintf(body, sizeof(body), "{\"server\":\"running\",\"filtering\":%s,\"activeScenario\":null}",
            state.filtering ? "true" : "false");
    }

    sendJsonResponse(client, 200, body);
}

static void handleScenarios(SOCKET client) {
    char body[REMOTE_RESPONSE_MAX];
    int offset = snprintf(body, sizeof(body), "{\"scenarios\":[");
    UINT ix;

    for (ix = 0; ix < getScenarioCount(); ++ix) {
        const Scenario *scenario = getScenarioByIndex(ix);
        if (scenario == NULL) {
            continue;
        }
        if (ix > 0 && offset < (int)sizeof(body) - 1) {
            body[offset++] = ',';
            body[offset] = '\0';
        }
        if (offset < (int)sizeof(body) - 1) {
            body[offset++] = '"';
            body[offset] = '\0';
        }
        offset = appendJsonEscaped(body, offset, sizeof(body), scenario->name);
        if (offset < (int)sizeof(body) - 1) {
            body[offset++] = '"';
            body[offset] = '\0';
        }
    }
    snprintf(body + offset, sizeof(body) - offset, "]}");
    sendJsonResponse(client, 200, body);
}

static void handleArm(SOCKET client, const char *path) {
    char scenarioName[SCENARIO_NAME_SIZE];
    char message[MSG_BUFSIZE];
    char body[REMOTE_RESPONSE_MAX];
    const char *nameStart = "/scenarios/";
    const char *nameEnd;
    int statusCode;

    path += strlen(nameStart);
    nameEnd = strstr(path, "/arm");
    if (nameEnd == NULL || *(nameEnd + 4) != '\0') {
        buildErrorJson(body, "Malformed arm path.");
        sendJsonResponse(client, 400, body);
        return;
    }

    {
        char encoded[SCENARIO_NAME_SIZE];
        size_t len = (size_t)(nameEnd - path);
        if (len == 0 || len >= sizeof(encoded)) {
            buildErrorJson(body, "Scenario name is invalid.");
            sendJsonResponse(client, 400, body);
            return;
        }
        memcpy(encoded, path, len);
        encoded[len] = '\0';
        if (!decodeUrlSegment(encoded, scenarioName)) {
            buildErrorJson(body, "Scenario name is invalid.");
            sendJsonResponse(client, 400, body);
            return;
        }
    }

    if (!submitUiCommand(REMOTE_COMMAND_ARM, scenarioName, &statusCode, message)) {
        buildErrorJson(body, message);
        sendJsonResponse(client, statusCode, body);
        return;
    }

    if (statusCode == 200) {
        int offset = snprintf(body, sizeof(body), "{\"ok\":true,\"activeScenario\":\"");
        offset = appendJsonEscaped(body, offset, sizeof(body), scenarioName);
        snprintf(body + offset, sizeof(body) - offset, "\"}");
        sendJsonResponse(client, 200, body);
    } else {
        buildErrorJson(body, message);
        sendJsonResponse(client, statusCode, body);
    }
}

static void handleDisarm(SOCKET client) {
    char message[MSG_BUFSIZE];
    char body[REMOTE_RESPONSE_MAX];
    int statusCode;

    if (!submitUiCommand(REMOTE_COMMAND_DISARM, NULL, &statusCode, message)) {
        buildErrorJson(body, message);
        sendJsonResponse(client, statusCode, body);
        return;
    }

    if (statusCode == 200) {
        snprintf(body, sizeof(body), "{\"ok\":true,\"activeScenario\":null}");
        sendJsonResponse(client, 200, body);
    } else {
        buildErrorJson(body, message);
        sendJsonResponse(client, statusCode, body);
    }
}

static void handleClient(SOCKET client) {
    char request[REMOTE_REQUEST_MAX];
    char method[16];
    char path[256];
    char version[16];
    char body[REMOTE_RESPONSE_MAX];

    if (!receiveHttpRequest(client, request)) {
        return;
    }

    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3) {
        buildErrorJson(body, "Malformed HTTP request.");
        sendJsonResponse(client, 400, body);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        handleHealth(client);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/scenarios") == 0) {
        handleScenarios(client);
        return;
    }
    if (strcmp(method, "POST") == 0 && strncmp(path, "/scenarios/", 11) == 0) {
        handleArm(client, path);
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/disarm") == 0) {
        handleDisarm(client);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        buildErrorJson(body, "HTTP method not supported.");
        sendJsonResponse(client, 405, body);
    } else {
        buildErrorJson(body, "Endpoint not found.");
        sendJsonResponse(client, 404, body);
    }
}

static DWORD WINAPI remoteServerThreadMain(LPVOID arg) {
    const RemoteServerConfig *config = (const RemoteServerConfig*)arg;
    struct sockaddr_in addr;
    fd_set readSet;
    struct timeval timeout;

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        LOG("Failed to create remote control socket.");
        InterlockedExchange(&remoteStartSucceeded, 0);
        SetEvent(serverStartupEvent);
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->port);
    addr.sin_addr.s_addr = inet_addr(config->bind);
    if (addr.sin_addr.s_addr == INADDR_NONE && strcmp(config->bind, "255.255.255.255") != 0) {
        LOG("Failed to parse remote bind address: %s", config->bind);
        closeSocketSafe(&listenSocket);
        InterlockedExchange(&remoteStartSucceeded, 0);
        SetEvent(serverStartupEvent);
        return 0;
    }

    if (bind(listenSocket, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        LOG("Failed to bind remote control socket. WSA=%d", WSAGetLastError());
        closeSocketSafe(&listenSocket);
        InterlockedExchange(&remoteStartSucceeded, 0);
        SetEvent(serverStartupEvent);
        return 0;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        LOG("Failed to listen on remote control socket. WSA=%d", WSAGetLastError());
        closeSocketSafe(&listenSocket);
        InterlockedExchange(&remoteStartSucceeded, 0);
        SetEvent(serverStartupEvent);
        return 0;
    }

    LOG("Remote control server listening on %s:%u", config->bind, config->port);
    InterlockedExchange(&remoteRunning, 1);
    InterlockedExchange(&remoteStartSucceeded, 1);
    SetEvent(serverStartupEvent);

    while (!stopRequested) {
        SOCKET client;

        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

        if (select(0, &readSet, NULL, NULL, &timeout) <= 0) {
            continue;
        }

        client = accept(listenSocket, NULL, NULL);
        if (client == INVALID_SOCKET) {
            continue;
        }

        {
            DWORD recvTimeout = 3000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));
        }
        handleClient(client);
        closesocket(client);
    }

    closeSocketSafe(&listenSocket);
    InterlockedExchange(&remoteRunning, 0);
    return 0;
}

BOOL remoteServerStart(HWND hWnd) {
    WSADATA wsaData;
    const RemoteServerConfig *config = getRemoteServerConfig();

    if (remoteThread != NULL) {
        return remoteServerIsRunning();
    }
    if (!config->enabled) {
        return FALSE;
    }

    ensureRemoteStateStorage();
    remoteWindow = hWnd;
    InterlockedExchange(&stopRequested, 0);
    InterlockedExchange(&remoteRunning, 0);
    InterlockedExchange(&remoteStartSucceeded, 0);
    ResetEvent(serverStartupEvent);

    if (!wsaStarted) {
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            LOG("Failed to start Winsock.");
            return FALSE;
        }
        wsaStarted = TRUE;
    }

    remoteThread = CreateThread(NULL, 0, remoteServerThreadMain, (LPVOID)config, 0, NULL);
    if (remoteThread == NULL) {
        LOG("Failed to create remote control thread.");
        if (wsaStarted) {
            WSACleanup();
            wsaStarted = FALSE;
        }
        return FALSE;
    }

    if (WaitForSingleObject(serverStartupEvent, 5000) != WAIT_OBJECT_0 || !remoteStartSucceeded) {
        WaitForSingleObject(remoteThread, INFINITE);
        CloseHandle(remoteThread);
        remoteThread = NULL;
        if (wsaStarted) {
            WSACleanup();
            wsaStarted = FALSE;
        }
        return FALSE;
    }

    return TRUE;
}

void remoteServerStop() {
    if (remoteThread == NULL) {
        return;
    }

    InterlockedExchange(&stopRequested, 1);
    closeSocketSafe(&listenSocket);
    WaitForSingleObject(remoteThread, INFINITE);
    CloseHandle(remoteThread);
    remoteThread = NULL;
    InterlockedExchange(&remoteRunning, 0);

    if (wsaStarted) {
        WSACleanup();
        wsaStarted = FALSE;
    }
}

BOOL remoteServerIsRunning() {
    return remoteRunning != 0;
}
