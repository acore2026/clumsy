#include <stdlib.h>
#include <string.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"

typedef struct {
    char key[UI_BINDING_KEY_SIZE];
    char field[16];
    char defaultValue[UI_BINDING_VALUE_SIZE];
    Ihandle *handle;
} UiBinding;

static UiBinding uiBindings[UI_BINDING_MAX];
static UINT uiBindingCount = 0;

typedef int (*IstateCallback)(Ihandle *ih, int state);

static UiBinding* findUiBinding(const char *key) {
    UINT ix;
    for (ix = 0; ix < uiBindingCount; ++ix) {
        if (_stricmp(uiBindings[ix].key, key) == 0) {
            return &uiBindings[ix];
        }
    }
    return NULL;
}

short calcChance(short chance) {
    // notice that here we made a copy of chance, so even though it's volatile it is still ok
    return (chance == 10000) || ((rand() % 10000) < chance);
}

static short resolutionSet = 0;

void startTimePeriod() {
    if (!resolutionSet) {
        // begin only fails when period out of range
        timeBeginPeriod(TIMER_RESOLUTION);
        resolutionSet = 1;
    }
}

void endTimePeriod() {
    if (resolutionSet) {
        timeEndPeriod(TIMER_RESOLUTION);
        resolutionSet = 0;
    }
}


// shared callbacks
int uiSyncChance(Ihandle *ih) {
    char valueBuf[8];
    float value = IupGetFloat(ih, "VALUE"), newValue = value;
    short *chancePtr = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    if (newValue > 100.0f) {
       newValue = 100.0f;
    } else if (newValue < 0) {
       newValue = 0.0f;
    }
    if (newValue != value) { // equality compare is fine since newValue is a copy of value
        sprintf(valueBuf, "%.1f", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        // put caret at end to enable editing while normalizing
        IupStoreAttribute(ih, "CARET", "10");
    }
    // and sync chance value
    InterlockedExchange16(chancePtr, (short)(newValue * 100));
    notifyUiStateEdited();
    return IUP_DEFAULT;
}

// shared callbacks
int uiSyncInt32(Ihandle *ih) {
    LONG *integerPointer = (LONG*)IupGetAttribute(ih, SYNCED_VALUE);
    const int maxValue = IupGetInt(ih, INTEGER_MAX);
    const int minValue = IupGetInt(ih, INTEGER_MIN);
    // normalize input into [min, max]
    int value = IupGetInt(ih, "VALUE"), newValue = value;
    char valueBuf[8];
    if (newValue > maxValue) {
        newValue = maxValue;
    } else if (newValue < minValue) {
        newValue = minValue;
    }
    // test for 0 as for empty input
    if (newValue != value && value != 0) {
        sprintf(valueBuf, "%d", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        // put caret at end to enable editing while normalizing
        IupStoreAttribute(ih, "CARET", "10");
    }
    // sync back
    InterlockedExchange(integerPointer, newValue);
    notifyUiStateEdited();
    return IUP_DEFAULT;
}

int uiSyncToggle(Ihandle *ih, int state) {
    short *togglePtr = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    InterlockedExchange16(togglePtr, I2S(state));
    notifyUiStateEdited();
    return IUP_DEFAULT;
}

int uiSyncInteger(Ihandle *ih) {
    short *integerPointer = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    const int maxValue = IupGetInt(ih, INTEGER_MAX);
    const int minValue = IupGetInt(ih, INTEGER_MIN);
    // normalize input into [min, max]
    int value = IupGetInt(ih, "VALUE"), newValue = value;
    char valueBuf[8];
    if (newValue > maxValue) {
        newValue = maxValue;
    } else if (newValue < minValue) {
        newValue = minValue;
    }
    // test for 0 as for empty input
    if (newValue != value && value != 0) {
        sprintf(valueBuf, "%d", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        // put caret at end to enable editing while normalizing
        IupStoreAttribute(ih, "CARET", "10");
    }
    // sync back
    InterlockedExchange16(integerPointer, (short)newValue);
    notifyUiStateEdited();
    return IUP_DEFAULT;
}

// naive fixed number of (short) * 0.01
int uiSyncFixed(Ihandle *ih) {
    short *fixedPointer = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    const float maxFixedValue = IupGetFloat(ih, FIXED_MAX);
    const float minFixedValue = IupGetFloat(ih, FIXED_MIN);
    float value = IupGetFloat(ih, "VALUE");
    float newValue = value;
    short fixValue;
    char valueBuf[8];
    if (newValue > maxFixedValue) {
        newValue = maxFixedValue;
    } else if (newValue < minFixedValue) {
        newValue = minFixedValue;
    }

    if (newValue != value && value != 0) {
        sprintf(valueBuf, "%.2f", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        // put caret at end to enable editing while normalizing
        IupStoreAttribute(ih, "CARET", "10");
    }
    // sync back
    fixValue = (short)(newValue / FIXED_EPSILON);
    InterlockedExchange16(fixedPointer, fixValue);
    notifyUiStateEdited();
    return IUP_DEFAULT;
}

BOOL applyValueToHandle(Ihandle *ih, const char *field, const char *value, const char *key) {
    Icallback cb;
    IstateCallback scb;

    if (ih == NULL || field == NULL || value == NULL) {
        return FALSE;
    }

    beginProgrammaticUiChange();
    IupSetAttribute(ih, field, value);

    cb = IupGetCallback(ih, "VALUECHANGED_CB");
    if (cb) {
        LOG("triggered VALUECHANGED_CB on key: %s", key ? key : "(direct)");
        cb(ih);
        endProgrammaticUiChange();
        return TRUE;
    }

    scb = (IstateCallback)IupGetCallback(ih, "ACTION");
    if (scb) {
        LOG("triggered ACTION on key: %s", key ? key : "(direct)");
        scb(ih, IupGetInt(ih, field));
    }
    endProgrammaticUiChange();
    return TRUE;
}

BOOL registerUiBinding(const char *key, Ihandle *ih, const char *field, const char *defaultValue) {
    UiBinding *binding;

    if (key == NULL || ih == NULL || field == NULL || defaultValue == NULL) {
        return FALSE;
    }

    binding = findUiBinding(key);
    if (binding == NULL) {
        if (uiBindingCount >= UI_BINDING_MAX) {
            LOG("UI binding limit reached, skipping key: %s", key);
            return FALSE;
        }
        binding = &uiBindings[uiBindingCount++];
        memset(binding, 0, sizeof(*binding));
    }

    strncpy(binding->key, key, UI_BINDING_KEY_SIZE - 1);
    strncpy(binding->field, field, sizeof(binding->field) - 1);
    strncpy(binding->defaultValue, defaultValue, UI_BINDING_VALUE_SIZE - 1);
    binding->handle = ih;
    return TRUE;
}

BOOL applyNamedUiValue(const char *key, const char *value) {
    UiBinding *binding = findUiBinding(key);
    if (binding == NULL) {
        return FALSE;
    }
    return applyValueToHandle(binding->handle, binding->field, value, key);
}

void resetNamedUiValues() {
    UINT ix;
    for (ix = 0; ix < uiBindingCount; ++ix) {
        applyValueToHandle(uiBindings[ix].handle, uiBindings[ix].field, uiBindings[ix].defaultValue, uiBindings[ix].key);
    }
}


// indicator icon, generated from scripts/im2carr.py
const unsigned char icon8x8[8*8] = {
    0, 0, 1, 1, 1, 1, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 0,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    0, 1, 1, 1, 1, 1, 1, 0,
    0, 0, 1, 1, 1, 1, 0, 0,
};

// parameterized setter
void setFromParameter(Ihandle *ih, const char *field, const char *key) {
    char* val = IupGetGlobal(key);
    if (val) {
        applyValueToHandle(ih, field, val, key);
    }
}

// parse arguments and set globals
// only checks for argument style, no extra validation is done
BOOL parseArgs(int argc, char* argv[]) {
    int ix = 0;
    char *key, *value;
    // begin parsing "--key value" args. 
    // notice that quoted arg with spaces in between is already handled by shell
    if (argc == 1) return 0;
    for (;;) {
        if (++ix >= argc) break;
        key = argv[ix];
        if (key[0] != '-' || key[1] != '-' || key[2] == '\0') {
            return 0;
        }
        key = &(key[2]); // skip "--"
        if (++ix >= argc) {
            return 0;
        }
        value = argv[ix];
        IupStoreGlobal(key, value);
        LOG("option: %s : %s", key, value);
    }

    return 1;
}
