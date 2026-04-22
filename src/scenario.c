#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <Windows.h>
#include "common.h"

#define SCENARIO_FILE "scenarios.ini"

static Scenario scenarios[SCENARIO_MAX_COUNT];
static UINT scenarioCount = 0;
static RemoteServerConfig remoteServerConfig = {0};

static char* trimLeft(char *text) {
    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }
    return text;
}

static void trimRight(char *text) {
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static char* trim(char *text) {
    text = trimLeft(text);
    trimRight(text);
    return text;
}

static void getExecutableSiblingPath(const char *fileName, char path[MSG_BUFSIZE]) {
    char *tail;
    GetModuleFileNameA(NULL, path, MSG_BUFSIZE);
    tail = strrchr(path, '\\');
    if (tail == NULL) {
        tail = strrchr(path, '/');
    }
    if (tail != NULL) {
        strcpy(tail + 1, fileName);
    }
}

static Scenario* addScenario(const char *name) {
    Scenario *scenario;
    if (scenarioCount >= SCENARIO_MAX_COUNT) {
        LOG("Scenario limit reached, skipping: %s", name);
        return NULL;
    }
    scenario = &scenarios[scenarioCount++];
    memset(scenario, 0, sizeof(*scenario));
    strncpy(scenario->name, name, sizeof(scenario->name) - 1);
    return scenario;
}

static ScenarioOption* findScenarioOption(Scenario *scenario, const char *key) {
    UINT ix;
    for (ix = 0; ix < scenario->optionCount; ++ix) {
        if (_stricmp(scenario->options[ix].key, key) == 0) {
            return &scenario->options[ix];
        }
    }
    return NULL;
}

static void setScenarioOption(Scenario *scenario, const char *key, const char *value) {
    ScenarioOption *option;
    if (scenario == NULL || key == NULL || value == NULL) {
        return;
    }

    option = findScenarioOption(scenario, key);
    if (option == NULL) {
        if (scenario->optionCount >= SCENARIO_OPTION_MAX) {
            LOG("Scenario option limit reached for %s, skipping key: %s", scenario->name, key);
            return;
        }
        option = &scenario->options[scenario->optionCount++];
        memset(option, 0, sizeof(*option));
        strncpy(option->key, key, sizeof(option->key) - 1);
    }

    strncpy(option->value, value, sizeof(option->value) - 1);
}

static BOOL scenarioNameExists(const char *name) {
    UINT ix;
    for (ix = 0; ix < scenarioCount; ++ix) {
        if (_stricmp(scenarios[ix].name, name) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL parseScenarioSectionName(const char *section, char outName[SCENARIO_NAME_SIZE]) {
    const char *quoteStart;
    const char *quoteEnd;

    if (_stricmp(section, "server") == 0) {
        return FALSE;
    }

    if (_strnicmp(section, "scenario", 8) != 0) {
        return FALSE;
    }

    quoteStart = strchr(section, '"');
    if (quoteStart == NULL) {
        return FALSE;
    }
    quoteEnd = strrchr(quoteStart + 1, '"');
    if (quoteEnd == NULL || quoteEnd <= quoteStart + 1) {
        return FALSE;
    }

    memset(outName, 0, SCENARIO_NAME_SIZE);
    strncpy(outName, quoteStart + 1, min((int)(quoteEnd - quoteStart - 1), SCENARIO_NAME_SIZE - 1));
    return outName[0] != '\0';
}

void loadScenarios() {
    FILE *file;
    char path[MSG_BUFSIZE];
    char line[512];
    enum { SECTION_NONE, SECTION_SERVER, SECTION_SCENARIO } section = SECTION_NONE;
    Scenario *currentScenario = NULL;
    BOOL serverSectionSeen = FALSE;
    UINT ix;

    memset(scenarios, 0, sizeof(scenarios));
    scenarioCount = 0;
    memset(&remoteServerConfig, 0, sizeof(remoteServerConfig));
    strcpy(remoteServerConfig.bind, "0.0.0.0");
    remoteServerConfig.port = 7878;

    getExecutableSiblingPath(SCENARIO_FILE, path);
    file = fopen(path, "r");
    if (file == NULL) {
        LOG("No scenarios file found at: %s", path);
        return;
    }

    LOG("Loading scenarios from: %s", path);

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = trim(line);
        char *equals;

        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }

        if (*text == '[') {
            char nameBuf[SCENARIO_NAME_SIZE];
            char *sectionEnd = strrchr(text, ']');
            if (sectionEnd == NULL) {
                section = SECTION_NONE;
                currentScenario = NULL;
                LOG("Ignoring malformed section header: %s", text);
                continue;
            }

            *sectionEnd = '\0';
            text = trim(text + 1);
            if (_stricmp(text, "server") == 0) {
                section = SECTION_SERVER;
                currentScenario = NULL;
                serverSectionSeen = TRUE;
                continue;
            }

            if (!parseScenarioSectionName(text, nameBuf)) {
                section = SECTION_NONE;
                currentScenario = NULL;
                LOG("Ignoring unknown section: %s", text);
                continue;
            }

            if (scenarioNameExists(nameBuf)) {
                section = SECTION_NONE;
                currentScenario = NULL;
                LOG("Ignoring duplicate scenario name: %s", nameBuf);
                continue;
            }

            currentScenario = addScenario(nameBuf);
            section = currentScenario ? SECTION_SCENARIO : SECTION_NONE;
            continue;
        }

        equals = strchr(text, '=');
        if (equals == NULL) {
            LOG("Ignoring malformed scenarios line: %s", text);
            continue;
        }

        *equals = '\0';
        text = trim(text);
        equals = trim(equals + 1);

        if (section == SECTION_SERVER) {
            if (_stricmp(text, "bind") == 0) {
                strncpy(remoteServerConfig.bind, equals, sizeof(remoteServerConfig.bind) - 1);
            } else if (_stricmp(text, "port") == 0) {
                long port = strtol(equals, NULL, 10);
                if (port > 0 && port <= 65535) {
                    remoteServerConfig.port = (unsigned short)port;
                } else {
                    LOG("Ignoring invalid remote port: %s", equals);
                }
            } else if (_stricmp(text, "token") == 0) {
                strncpy(remoteServerConfig.token, equals, sizeof(remoteServerConfig.token) - 1);
            } else {
                LOG("Ignoring unknown remote server key: %s", text);
            }
        } else if (section == SECTION_SCENARIO && currentScenario != NULL) {
            if (_stricmp(text, "filter") == 0) {
                strncpy(currentScenario->filter, equals, sizeof(currentScenario->filter) - 1);
            } else {
                setScenarioOption(currentScenario, text, equals);
            }
        }
    }

    fclose(file);

    for (ix = 0; ix < scenarioCount;) {
        if (scenarios[ix].filter[0] == '\0') {
            UINT moveIx;
            LOG("Ignoring scenario without filter: %s", scenarios[ix].name);
            for (moveIx = ix + 1; moveIx < scenarioCount; ++moveIx) {
                scenarios[moveIx - 1] = scenarios[moveIx];
            }
            memset(&scenarios[scenarioCount - 1], 0, sizeof(scenarios[0]));
            --scenarioCount;
        } else {
            ++ix;
        }
    }

    if (serverSectionSeen && remoteServerConfig.bind[0] != '\0') {
        remoteServerConfig.enabled = TRUE;
    } else {
        if (serverSectionSeen) {
            LOG("Remote server config incomplete, HTTP control disabled.");
        }
        remoteServerConfig.enabled = FALSE;
    }

    LOG("Loaded %u scenarios.", scenarioCount);
}

const Scenario* getScenarioByName(const char *name) {
    UINT ix;
    if (name == NULL) {
        return NULL;
    }
    for (ix = 0; ix < scenarioCount; ++ix) {
        if (_stricmp(scenarios[ix].name, name) == 0) {
            return &scenarios[ix];
        }
    }
    return NULL;
}

const Scenario* getScenarioByIndex(UINT index) {
    return index < scenarioCount ? &scenarios[index] : NULL;
}

UINT getScenarioCount() {
    return scenarioCount;
}

const RemoteServerConfig* getRemoteServerConfig() {
    return &remoteServerConfig;
}
