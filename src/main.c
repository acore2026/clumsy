#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"

// ! the order decides which module get processed first
Module* modules[MODULE_CNT] = {
    &lagModule,
    &dropModule,
    &throttleModule,
    &dupModule,
    &oodModule,
    &tamperModule,
    &resetModule,
	&bandwidthModule,
};

volatile short sendState = SEND_STATUS_NONE;
static volatile LONG programmaticUiDepth = 0;
static BOOL filteringActive = FALSE;
static char activeScenario[SCENARIO_NAME_SIZE] = {0};
static HWND dialogHwnd = NULL;
static WNDPROC originalDialogWndProc = NULL;
static Ihandle *moduleToggleHandles[MODULE_CNT] = {0};
static Ihandle *moduleControlsHandles[MODULE_CNT] = {0};

// global iup handlers
static Ihandle *dialog, *topFrame, *remoteFrame, *bottomFrame; 
static Ihandle *statusLabel;
static Ihandle *filterText, *filterButton;
Ihandle *filterSelectList;
static Ihandle *remoteServerButton, *remoteServerStatusLabel, *scenarioSelectList, *armScenarioButton, *disarmScenarioButton, *armedScenarioNameLabel, *armedScenarioContentText;
// timer to update icons
static Ihandle *stateIcon;
static Ihandle *timer;
static Ihandle *timeout = NULL;

void showStatus(const char *line);
static int uiOnDialogShow(Ihandle *ih, int state);
static int uiStopCb(Ihandle *ih);
static int uiStartCb(Ihandle *ih);
static int uiTimerCb(Ihandle *ih);
static int uiTimeoutCb(Ihandle *ih);
static int uiListSelectCb(Ihandle *ih, char *text, int item, int state);
static int uiFilterTextCb(Ihandle *ih);
static void uiSetupModule(Module *module, Ihandle *parent);
static BOOL startFilteringWithCurrentState(char buf[MSG_BUFSIZE]);
static void stopFilteringCurrentRun();
static void applyScenarioToUiAndState(const Scenario *scenario);
static void updateRemoteRuntimeState();
static void setActiveScenario(const char *name);
static void clearActiveScenario();
static BOOL installRemoteMessageWindow(HWND hWnd);
static LRESULT CALLBACK dialogWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static BOOL isRemoteControlMode();
static void refreshUiLockState();
static void refreshRemoteUi();
static void updateArmedScenarioDisplay();
static int uiRemoteServerToggleCb(Ihandle *ih);
static int uiScenarioListSelectCb(Ihandle *ih, char *text, int item, int state);
static int uiArmScenarioCb(Ihandle *ih);
static int uiDisarmScenarioCb(Ihandle *ih);

// serializing config files using a stupid custom format
#define CONFIG_FILE "config.txt"
#define CONFIG_MAX_RECORDS 64
#define CONFIG_BUF_SIZE 4096
typedef struct {
    char* filterName;
    char* filterValue;
} filterRecord;
UINT filtersSize;
filterRecord filters[CONFIG_MAX_RECORDS] = {0};
char configBuf[CONFIG_BUF_SIZE+2]; // add some padding to write \n
BOOL parameterized = 0; // parameterized flag, means reading args from command line

// loading up filters and fill in
void loadConfig() {
    char path[MSG_BUFSIZE];
    char *p;
    FILE *f;
    GetModuleFileName(NULL, path, MSG_BUFSIZE);
    LOG("Executable path: %s", path);
    p = strrchr(path, '\\');
    if (p == NULL) p = strrchr(path, '/'); // holy shit
    strcpy(p+1, CONFIG_FILE);
    LOG("Config path: %s", path);
    f = fopen(path, "r");
    if (f) {
        size_t len;
        char *current, *last;
        len = fread(configBuf, sizeof(char), CONFIG_BUF_SIZE, f);
        if (len == CONFIG_BUF_SIZE) {
            LOG("Config file is larger than %d bytes, get truncated.", CONFIG_BUF_SIZE);
        }
        // always patch in a newline at the end to ease parsing
        configBuf[len] = '\n';
        configBuf[len+1] = '\0';

        // parse out the kv pairs. isn't quite safe
        filtersSize = 0;
        last = current = configBuf;
        do {
            // eat up empty lines
EAT_SPACE:  while (isspace(*current)) { ++current; }
            if (*current == '#') {
                current = strchr(current, '\n');
                if (!current) break;
                current = current + 1;
                goto EAT_SPACE;
            }

            // now we can start
            last = current;
            current = strchr(last, ':');
            if (!current) break;
            *current = '\0';
            filters[filtersSize].filterName = last;
            current += 1;
            while (isspace(*current)) { ++current; } // eat potential space after :
            last = current;
            current = strchr(last, '\n');
            if (!current) break;
            filters[filtersSize].filterValue = last;
            *current = '\0';
            if (*(current-1) == '\r') *(current-1) = 0;
            last = current = current + 1;
            ++filtersSize;
        } while (last && last - configBuf < CONFIG_BUF_SIZE);
        LOG("Loaded %u records.", filtersSize);
    }

    if (!f || filtersSize == 0)
    {
        LOG("Failed to load from config. Fill in a simple one.");
        // config is missing or ill-formed. fill in some simple ones
        filters[filtersSize].filterName = "loopback packets";
        filters[filtersSize].filterValue = "outbound and ip.DstAddr >= 127.0.0.1 and ip.DstAddr <= 127.255.255.255";
        filtersSize = 1;
    }
}

void beginProgrammaticUiChange() {
    InterlockedIncrement(&programmaticUiDepth);
}

void endProgrammaticUiChange() {
    InterlockedDecrement(&programmaticUiDepth);
}

static void updateRemoteRuntimeState() {
    remoteServerSetRuntimeState(filteringActive, activeScenario[0] ? activeScenario : NULL);
}

static void setActiveScenario(const char *name) {
    UINT ix;
    activeScenario[0] = '\0';
    if (name != NULL) {
        strncpy(activeScenario, name, sizeof(activeScenario) - 1);
    }
    if (scenarioSelectList != NULL && name != NULL) {
        for (ix = 0; ix < getScenarioCount(); ++ix) {
            const Scenario *scenario = getScenarioByIndex(ix);
            if (scenario != NULL && _stricmp(scenario->name, name) == 0) {
                char ixBuf[8];
                sprintf(ixBuf, "%d", ix + 1);
                IupSetAttribute(scenarioSelectList, "VALUE", ixBuf);
                break;
            }
        }
    }
    updateRemoteRuntimeState();
    updateArmedScenarioDisplay();
}

static void clearActiveScenario() {
    if (activeScenario[0] == '\0') {
        return;
    }
    activeScenario[0] = '\0';
    updateRemoteRuntimeState();
    updateArmedScenarioDisplay();
}

static void updateArmedScenarioDisplay() {
    const Scenario *scenario;
    char content[4096];
    char title[128];
    int offset = 0;
    UINT ix;

    if (armedScenarioNameLabel == NULL || armedScenarioContentText == NULL) {
        return;
    }

    if (activeScenario[0] == '\0') {
        IupStoreAttribute(armedScenarioNameLabel, "TITLE", "Armed Scenario: none");
        IupStoreAttribute(armedScenarioContentText, "VALUE", "No scenario is currently armed.");
        return;
    }

    scenario = getScenarioByName(activeScenario);
    if (scenario == NULL) {
        IupStoreAttribute(armedScenarioNameLabel, "TITLE", "Armed Scenario: unavailable");
        IupStoreAttribute(armedScenarioContentText, "VALUE", "The active scenario was not found in the loaded scenarios.");
        return;
    }

    snprintf(title, sizeof(title), "Armed Scenario: %s", activeScenario);
    IupStoreAttribute(armedScenarioNameLabel, "TITLE", title);
    offset += snprintf(content + offset, sizeof(content) - offset, "filter = %s\r\n", scenario->filter);
    for (ix = 0; ix < scenario->optionCount && offset < (int)sizeof(content) - 1; ++ix) {
        offset += snprintf(
            content + offset,
            sizeof(content) - offset,
            "%s = %s\r\n",
            scenario->options[ix].key,
            scenario->options[ix].value
        );
    }
    IupStoreAttribute(armedScenarioContentText, "VALUE", content);
}

static void refreshRemoteUi() {
    const RemoteServerConfig *config = getRemoteServerConfig();
    char title[128];
    char status[256];

    if (remoteServerButton == NULL || remoteServerStatusLabel == NULL || scenarioSelectList == NULL || armScenarioButton == NULL || disarmScenarioButton == NULL) {
        return;
    }

    if (config == NULL || !config->enabled) {
        IupSetAttribute(remoteServerButton, "ACTIVE", "NO");
        IupStoreAttribute(remoteServerButton, "TITLE", "HTTP Unavailable");
        IupStoreAttribute(remoteServerStatusLabel, "TITLE", "HTTP control disabled: missing or invalid [server] config in scenarios.ini.");
    } else {
        snprintf(title, sizeof(title), "%s HTTP Server", remoteServerIsRunning() ? "Stop" : "Start");
        snprintf(
            status,
            sizeof(status),
            "HTTP server %s on %s:%u",
            remoteServerIsRunning() ? "running" : "stopped",
            config->bind,
            config->port
        );
        IupSetAttribute(remoteServerButton, "ACTIVE", "YES");
        IupStoreAttribute(remoteServerButton, "TITLE", title);
        IupStoreAttribute(remoteServerStatusLabel, "TITLE", status);
    }

    IupSetAttribute(scenarioSelectList, "ACTIVE", getScenarioCount() > 0 ? "YES" : "NO");
    IupSetAttribute(armScenarioButton, "ACTIVE", getScenarioCount() > 0 ? "YES" : "NO");
    IupSetAttribute(disarmScenarioButton, "ACTIVE", "YES");
    updateArmedScenarioDisplay();
}

static BOOL isRemoteControlMode() {
    return remoteServerIsRunning();
}

static void refreshUiLockState() {
    UINT ix;
    BOOL remoteMode = isRemoteControlMode();

    IupSetAttribute(filterText, "ACTIVE", remoteMode ? "NO" : (filteringActive ? "NO" : "YES"));
    IupSetAttribute(filterSelectList, "ACTIVE", remoteMode ? "NO" : "YES");

    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (moduleToggleHandles[ix] != NULL) {
            IupSetAttribute(moduleToggleHandles[ix], "ACTIVE", remoteMode ? "NO" : "YES");
        }
        if (moduleControlsHandles[ix] != NULL) {
            if (remoteMode) {
                IupSetAttribute(moduleControlsHandles[ix], "ACTIVE", "NO");
            } else {
                IupSetAttribute(moduleControlsHandles[ix], "ACTIVE", *(modules[ix]->enabledFlag) ? "YES" : "NO");
            }
        }
    }
    refreshRemoteUi();
}

void notifyUiStateEdited() {
    if (programmaticUiDepth > 0) {
        return;
    }
    clearActiveScenario();
}

BOOL isFilteringActive() {
    return filteringActive;
}

void init(int argc, char* argv[]) {
    UINT ix;
    Ihandle *topVbox, *remoteVbox, *remoteControlHbox, *remoteScenarioHbox, *bottomVbox, *dialogVBox, *controlHbox;
    Ihandle *noneIcon, *doingIcon, *errorIcon;
    char* arg_value = NULL;

    // fill in config
    loadConfig();
    loadScenarios();

    // iup inits
    IupOpen(&argc, &argv);

    // this is so easy to get wrong so it's pretty worth noting in the program
    statusLabel = IupLabel("NOTICE: When capturing localhost (loopback) packets, you CAN'T include inbound criteria.\n"
        "Filters like 'udp' need to be 'udp and outbound' to work. See readme for more info.");
    IupSetAttribute(statusLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(statusLabel, "PADDING", "8x8");

    topFrame = IupFrame(
        topVbox = IupVbox(
            filterText = IupText(NULL),
            controlHbox = IupHbox(
                stateIcon = IupLabel(NULL),
                filterButton = IupButton("Start", NULL),
                IupFill(),
                IupLabel("Presets:  "),
                filterSelectList = IupList(NULL),
                NULL
            ),
            NULL
        )
    );

    remoteFrame = IupFrame(
        remoteVbox = IupVbox(
            remoteControlHbox = IupHbox(
                remoteServerButton = IupButton("Start HTTP Server", NULL),
                IupFill(),
                remoteServerStatusLabel = IupLabel("HTTP server not initialized."),
                NULL
            ),
            remoteScenarioHbox = IupHbox(
                IupLabel("Scenarios:"),
                scenarioSelectList = IupList(NULL),
                armScenarioButton = IupButton("Arm", NULL),
                disarmScenarioButton = IupButton("Disarm", NULL),
                NULL
            ),
            armedScenarioNameLabel = IupLabel("Armed Scenario: none"),
            armedScenarioContentText = IupText(NULL),
            NULL
        )
    );

    // parse arguments and set globals *before* setting up UI.
    // arguments can be read and set after callbacks are setup
    // FIXME as Release is built as WindowedApp, stdout/stderr won't show
    LOG("argc: %d", argc);
    if (argc > 1) {
        if (!parseArgs(argc, argv)) {
            fprintf(stderr, "invalid argument count. ensure you're using options as \"--drop on\"");
            exit(-1); // fail fast.
        }
        parameterized = 1;
    }

    IupSetAttribute(topFrame, "TITLE", "Filtering");
    IupSetAttribute(topFrame, "EXPAND", "HORIZONTAL");
    IupSetAttribute(filterText, "EXPAND", "HORIZONTAL");
    IupSetCallback(filterText, "VALUECHANGED_CB", (Icallback)uiFilterTextCb);
    IupSetAttribute(filterButton, "PADDING", "8x");
    IupSetCallback(filterButton, "ACTION", uiStartCb);
    IupSetAttribute(topVbox, "NCMARGIN", "4x4");
    IupSetAttribute(topVbox, "NCGAP", "4x2");
    IupSetAttribute(controlHbox, "ALIGNMENT", "ACENTER");

    IupSetAttribute(remoteFrame, "TITLE", "Remote Control");
    IupSetAttribute(remoteFrame, "EXPAND", "HORIZONTAL");
    IupSetAttribute(remoteVbox, "NCMARGIN", "4x4");
    IupSetAttribute(remoteVbox, "NCGAP", "4x2");
    IupSetAttribute(remoteControlHbox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(remoteScenarioHbox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(remoteServerButton, "PADDING", "8x");
    IupSetCallback(remoteServerButton, "ACTION", uiRemoteServerToggleCb);
    IupSetAttribute(scenarioSelectList, "VISIBLECOLUMNS", "28");
    IupSetAttribute(scenarioSelectList, "DROPDOWN", "YES");
    IupSetCallback(scenarioSelectList, "ACTION", (Icallback)uiScenarioListSelectCb);
    for (ix = 0; ix < getScenarioCount(); ++ix) {
        const Scenario *scenario = getScenarioByIndex(ix);
        char ixBuf[8];
        if (scenario == NULL) {
            continue;
        }
        sprintf(ixBuf, "%d", ix + 1);
        IupStoreAttribute(scenarioSelectList, ixBuf, scenario->name);
    }
    if (getScenarioCount() > 0) {
        IupSetAttribute(scenarioSelectList, "VALUE", "1");
    } else {
        IupSetAttribute(scenarioSelectList, "VALUE", "0");
    }
    IupSetAttribute(armScenarioButton, "PADDING", "8x");
    IupSetAttribute(disarmScenarioButton, "PADDING", "8x");
    IupSetCallback(armScenarioButton, "ACTION", uiArmScenarioCb);
    IupSetCallback(disarmScenarioButton, "ACTION", uiDisarmScenarioCb);
    IupSetAttribute(armedScenarioNameLabel, "PADDING", "4x2");
    IupSetAttribute(armedScenarioContentText, "MULTILINE", "YES");
    IupSetAttribute(armedScenarioContentText, "READONLY", "YES");
    IupSetAttribute(armedScenarioContentText, "EXPAND", "HORIZONTAL");
    IupSetAttribute(armedScenarioContentText, "VISIBLELINES", "6");
    IupSetAttribute(armedScenarioContentText, "VALUE", "No scenario is currently armed.");

    // setup state icon
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");
    IupSetAttribute(stateIcon, "PADDING", "4x");

    // fill in options and setup callback
    IupSetAttribute(filterSelectList, "VISIBLECOLUMNS", "24");
    IupSetAttribute(filterSelectList, "DROPDOWN", "YES");
    for (ix = 0; ix < filtersSize; ++ix) {
        char ixBuf[4];
        sprintf(ixBuf, "%d", ix+1); // ! staring from 1, following lua indexing
        IupStoreAttribute(filterSelectList, ixBuf, filters[ix].filterName);
    }
    IupSetAttribute(filterSelectList, "VALUE", "1");
    IupSetCallback(filterSelectList, "ACTION", (Icallback)uiListSelectCb);
    // set filter text value since the callback won't take effect before main loop starts
    IupSetAttribute(filterText, "VALUE", filters[0].filterValue);

    // functionalities frame 
    bottomFrame = IupFrame(
        bottomVbox = IupVbox(
            NULL
        )
    );
    IupSetAttribute(bottomFrame, "TITLE", "Functions");
    IupSetAttribute(bottomVbox, "NCMARGIN", "4x4");
    IupSetAttribute(bottomVbox, "NCGAP", "4x2");

    // create icons
    noneIcon = IupImage(8, 8, icon8x8);
    doingIcon = IupImage(8, 8, icon8x8);
    errorIcon = IupImage(8, 8, icon8x8);
    IupSetAttribute(noneIcon, "0", "BGCOLOR");
    IupSetAttribute(noneIcon, "1", "224 224 224");
    IupSetAttribute(doingIcon, "0", "BGCOLOR");
    IupSetAttribute(doingIcon, "1", "109 170 44");
    IupSetAttribute(errorIcon, "0", "BGCOLOR");
    IupSetAttribute(errorIcon, "1", "208 70 72");
    IupSetHandle("none_icon", noneIcon);
    IupSetHandle("doing_icon", doingIcon);
    IupSetHandle("error_icon", errorIcon);

    // setup module uis
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        uiSetupModule(*(modules+ix), bottomVbox);
    }

    // dialog
    dialog = IupDialog(
        dialogVBox = IupVbox(
            topFrame,
            remoteFrame,
            bottomFrame,
            statusLabel,
            NULL
        )
    );

    IupSetAttribute(dialog, "TITLE", "clumsy " CLUMSY_VERSION);
    IupSetAttribute(dialog, "SIZE", "560x420");
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetCallback(dialog, "SHOW_CB", (Icallback)uiOnDialogShow);


    // global layout settings to affect childrens
    IupSetAttribute(dialogVBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(dialogVBox, "NCMARGIN", "4x4");
    IupSetAttribute(dialogVBox, "NCGAP", "4x2");

    // setup timer
    timer = IupTimer();
    IupSetAttribute(timer, "TIME", STR(ICON_UPDATE_MS));
    IupSetCallback(timer, "ACTION_CB", uiTimerCb);

    // setup timeout of program
    arg_value = IupGetGlobal("timeout");
    if(arg_value != NULL)
    {
        char valueBuf[16];
        sprintf(valueBuf, "%s000", arg_value);  // convert from seconds to milliseconds

        timeout = IupTimer();
        IupStoreAttribute(timeout, "TIME", valueBuf);
        IupSetCallback(timeout, "ACTION_CB", uiTimeoutCb);
        IupSetAttribute(timeout, "RUN", "YES");
    }

    refreshUiLockState();
}

void startup() {
    // initialize seed
    srand((unsigned int)time(NULL));

    // kickoff event loops
    IupShowXY(dialog, IUP_CENTER, IUP_CENTER);
    IupMainLoop();
    // ! main loop won't return until program exit
}

void cleanup() {
    remoteServerStop();

    IupDestroy(timer);
    if (timeout) {
        IupDestroy(timeout);
    }

    IupClose();
    endTimePeriod(); // try close if not closing
}

// ui logics
void showStatus(const char *line) {
    IupStoreAttribute(statusLabel, "TITLE", line); 
}

// in fact only 32bit binary would run on 64 bit os
// if this happens pop out message box and exit
static BOOL check32RunningOn64(HWND hWnd) {
    BOOL is64ret;
    // consider IsWow64Process return value
    if (IsWow64Process(GetCurrentProcess(), &is64ret) && is64ret) {
        MessageBox(hWnd, (LPCSTR)"You're running 32bit clumsy on 64bit Windows, which wouldn't work. Please use the 64bit clumsy version.",
            (LPCSTR)"Aborting", MB_OK);
        return TRUE;
    }
    return FALSE;
}

static BOOL checkIsRunning() {
    //It will be closed and destroyed when programm terminates (according to MSDN).
    HANDLE hStartEvent = CreateEventW(NULL, FALSE, FALSE, L"Global\\CLUMSY_IS_RUNNING_EVENT_NAME");

    if (hStartEvent == NULL)
        return TRUE;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hStartEvent);
        hStartEvent = NULL;
        return TRUE;
    }

    return FALSE;
}

static LRESULT CALLBACK dialogWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLUMSY_REMOTE_COMMAND) {
        remoteServerDispatchCommand();
        return 0;
    }
    return CallWindowProc(originalDialogWndProc, hWnd, message, wParam, lParam);
}

static BOOL installRemoteMessageWindow(HWND hWnd) {
    if (originalDialogWndProc != NULL) {
        return TRUE;
    }

    dialogHwnd = hWnd;
    originalDialogWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)dialogWindowProc);
    return originalDialogWndProc != NULL;
}


static int uiOnDialogShow(Ihandle *ih, int state) {
    // only need to process on show
    HWND hWnd;
    BOOL exit;
    HICON icon;
    HINSTANCE hInstance;
    if (state != IUP_SHOW) return IUP_DEFAULT;
    hWnd = (HWND)IupGetAttribute(ih, "HWND");
    hInstance = GetModuleHandle(NULL);

    // set application icon
    icon = LoadIcon(hInstance, "CLUMSY_ICON");
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);

    exit = checkIsRunning();
    if (exit) {
        MessageBox(hWnd, (LPCSTR)"Theres' already an instance of clumsy running.",
            (LPCSTR)"Aborting", MB_OK);
        return IUP_CLOSE;
    }

#ifdef _WIN32
    exit = check32RunningOn64(hWnd);
    if (exit) {
        return IUP_CLOSE;
    }
#endif

    // try elevate and decides whether to exit
    exit = tryElevate(hWnd, parameterized);

    if (!exit && parameterized) {
        setFromParameter(filterText, "VALUE", "filter");
        LOG("is parameterized, start filtering upon execution.");
        uiStartCb(filterButton);
    }

    if (!exit && installRemoteMessageWindow(hWnd)) {
        if (getRemoteServerConfig()->enabled && !remoteServerStart(hWnd)) {
            showStatus("Failed to start HTTP server. Check scenarios.ini server settings and whether the port is already in use.");
        }
        updateRemoteRuntimeState();
        refreshUiLockState();
    }

    return exit ? IUP_CLOSE : IUP_DEFAULT;
}

static BOOL startFilteringWithCurrentState(char buf[MSG_BUFSIZE]) {
    if (divertStart(IupGetAttribute(filterText, "VALUE"), buf) == 0) {
        showStatus(buf);
        filteringActive = FALSE;
        updateRemoteRuntimeState();
        return FALSE;
    }

    // successfully started
    showStatus("Started filtering. Enable functionalities to take effect.");
    IupSetAttribute(filterText, "ACTIVE", "NO");
    IupSetAttribute(filterButton, "TITLE", "Stop");
    IupSetCallback(filterButton, "ACTION", uiStopCb);
    IupSetAttribute(timer, "RUN", "YES");
    filteringActive = TRUE;
    updateRemoteRuntimeState();
    refreshUiLockState();
    return TRUE;
}

static int uiStartCb(Ihandle *ih) {
    char buf[MSG_BUFSIZE];
    UNREFERENCED_PARAMETER(ih);
    notifyUiStateEdited();
    startFilteringWithCurrentState(buf);
    return IUP_DEFAULT;
}

static void stopFilteringCurrentRun() {
    int ix;

    if (!filteringActive) {
        return;
    }

    // try stopping
    IupSetAttribute(filterButton, "ACTIVE", "NO");
    IupFlush(); // flush to show disabled state
    divertStop();

    IupSetAttribute(filterButton, "TITLE", "Start");
    IupSetAttribute(filterButton, "ACTIVE", "YES");
    IupSetCallback(filterButton, "ACTION", uiStartCb);

    // stop timer and clean up icons
    IupSetAttribute(timer, "RUN", "NO");
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        modules[ix]->processTriggered = 0; // use = here since is threads already stopped
        IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
    }
    sendState = SEND_STATUS_NONE;
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");

    showStatus("Stopped. To begin again, edit criteria and click Start.");
    filteringActive = FALSE;
    updateRemoteRuntimeState();
    refreshUiLockState();
}

static int uiStopCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    notifyUiStateEdited();
    stopFilteringCurrentRun();
    return IUP_DEFAULT;
}

static int uiToggleControls(Ihandle *ih, int state) {
    Ihandle *controls = (Ihandle*)IupGetAttribute(ih, CONTROLS_HANDLE);
    short *target = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    int controlsActive = IupGetInt(controls, "ACTIVE");
    notifyUiStateEdited();
    if (controlsActive && !state) {
        IupSetAttribute(controls, "ACTIVE", "NO");
        InterlockedExchange16(target, I2S(state));
    } else if (!controlsActive && state) {
        IupSetAttribute(controls, "ACTIVE", "YES");
        InterlockedExchange16(target, I2S(state));
    }

    return IUP_DEFAULT;
}

static int uiTimerCb(Ihandle *ih) {
    int ix;
    UNREFERENCED_PARAMETER(ih);
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (modules[ix]->processTriggered) {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "doing_icon");
            InterlockedAnd16(&(modules[ix]->processTriggered), 0);
        } else {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
        }
    }

    // update global send status icon
    switch (sendState)
    {
    case SEND_STATUS_NONE:
        IupSetAttribute(stateIcon, "IMAGE", "none_icon");
        break;
    case SEND_STATUS_SEND:
        IupSetAttribute(stateIcon, "IMAGE", "doing_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    case SEND_STATUS_FAIL:
        IupSetAttribute(stateIcon, "IMAGE", "error_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    }

    return IUP_DEFAULT;
}

static int uiTimeoutCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    return IUP_CLOSE;
 }

static int uiListSelectCb(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(text);
    UNREFERENCED_PARAMETER(ih);
    if (state == 1) {
        IupSetAttribute(filterText, "VALUE", filters[item-1].filterValue);
        uiFilterTextCb(filterText);
    }
    return IUP_DEFAULT;
}

static int uiFilterTextCb(Ihandle *ih)  {
    UNREFERENCED_PARAMETER(ih);
    // unselect list
    IupSetAttribute(filterSelectList, "VALUE", "0");
    notifyUiStateEdited();
    return IUP_DEFAULT;
}

static int uiRemoteServerToggleCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);

    if (!remoteServerIsRunning()) {
        if (!remoteServerStart(dialogHwnd)) {
            showStatus("Failed to start HTTP server. Check scenarios.ini server settings and whether the port is already in use.");
        } else {
            updateRemoteRuntimeState();
            showStatus("HTTP server started.");
        }
    } else {
        remoteServerStop();
        showStatus("HTTP server stopped.");
    }

    refreshUiLockState();
    return IUP_DEFAULT;
}

static int uiScenarioListSelectCb(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(ih);
    UNREFERENCED_PARAMETER(text);
    UNREFERENCED_PARAMETER(item);
    UNREFERENCED_PARAMETER(state);
    return IUP_DEFAULT;
}

static int uiArmScenarioCb(Ihandle *ih) {
    char buf[MSG_BUFSIZE];
    const char *selectedScenario;
    int result;

    UNREFERENCED_PARAMETER(ih);

    selectedScenario = IupGetAttribute(scenarioSelectList, "VALUESTRING");
    if (selectedScenario == NULL || *selectedScenario == '\0') {
        showStatus("Select a scenario to arm.");
        return IUP_DEFAULT;
    }

    result = armScenarioByName(selectedScenario, buf, MSG_BUFSIZE);
    showStatus(buf);
    if (result == 200) {
        refreshUiLockState();
    }
    return IUP_DEFAULT;
}

static int uiDisarmScenarioCb(Ihandle *ih) {
    char buf[MSG_BUFSIZE];

    UNREFERENCED_PARAMETER(ih);

    disarmActiveScenario(buf, MSG_BUFSIZE);
    showStatus(buf);
    refreshUiLockState();
    return IUP_DEFAULT;
}

static void uiSetupModule(Module *module, Ihandle *parent) {
    Ihandle *groupBox, *toggle, *controls, *icon;
    UINT ix;
    groupBox = IupHbox(
        icon = IupLabel(NULL),
        toggle = IupToggle(module->displayName, NULL),
        IupFill(),
        controls = module->setupUIFunc(),
        NULL
    );
    IupSetAttribute(groupBox, "EXPAND", "HORIZONTAL");
    IupSetAttribute(groupBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(controls, "ALIGNMENT", "ACENTER");
    IupAppend(parent, groupBox);

    // set controls as attribute to toggle and enable toggle callback
    IupSetCallback(toggle, "ACTION", (Icallback)uiToggleControls);
    IupSetAttribute(toggle, CONTROLS_HANDLE, (char*)controls);
    IupSetAttribute(toggle, SYNCED_VALUE, (char*)module->enabledFlag);
    registerUiBinding(module->shortName, toggle, "VALUE", "OFF");
    IupSetAttribute(controls, "ACTIVE", "NO"); // startup as inactive
    IupSetAttribute(controls, "NCGAP", "4"); // startup as inactive
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (modules[ix] == module) {
            moduleToggleHandles[ix] = toggle;
            moduleControlsHandles[ix] = controls;
            break;
        }
    }

    // set default icon
    IupSetAttribute(icon, "IMAGE", "none_icon");
    IupSetAttribute(icon, "PADDING", "4x");
    module->iconHandle = icon;

    // parameterize toggle
    if (parameterized) {
        setFromParameter(toggle, "VALUE", module->shortName);
    }
}

static void applyScenarioToUiAndState(const Scenario *scenario) {
    UINT ix;

    resetNamedUiValues();
    resetLagScenarioConfig();
    applyValueToHandle(filterText, "VALUE", scenario->filter, "filter");

    for (ix = 0; ix < scenario->optionCount; ++ix) {
        if (applyLagScenarioOption(scenario->options[ix].key, scenario->options[ix].value)) {
            continue;
        }
        if (!applyNamedUiValue(scenario->options[ix].key, scenario->options[ix].value)) {
            LOG("Ignoring unknown scenario key: %s", scenario->options[ix].key);
        }
    }
    finalizeLagScenarioConfig();
}

int armScenarioByName(const char *name, char buf[], UINT bufSize) {
    const Scenario *scenario = getScenarioByName(name);

    if (scenario == NULL) {
        snprintf(buf, bufSize, "Scenario not found: %s", name ? name : "(null)");
        return 404;
    }

    if (filteringActive) {
        stopFilteringCurrentRun();
    }

    applyScenarioToUiAndState(scenario);
    if (!startFilteringWithCurrentState(buf)) {
        clearActiveScenario();
        return 409;
    }

    setActiveScenario(scenario->name);
    snprintf(buf, bufSize, "Armed scenario: %s", scenario->name);
    return 200;
}

int disarmActiveScenario(char buf[], UINT bufSize) {
    if (filteringActive) {
        stopFilteringCurrentRun();
    }
    clearActiveScenario();
    snprintf(buf, bufSize, "Disarmed active scenario.");
    return 200;
}

int main(int argc, char* argv[]) {
    LOG("Is Run As Admin: %d", IsRunAsAdmin());
    LOG("Is Elevated: %d", IsElevated());
    init(argc, argv);
    startup();
    cleanup();
    return 0;
}
