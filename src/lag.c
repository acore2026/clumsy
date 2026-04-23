// lagging packets
#include <stdlib.h>
#include <string.h>
#include "iup.h"
#include "common.h"
#define NAME "lag"
#define LAG_MIN "0"
#define LAG_MAX "15000"
#define KEEP_AT_MOST 2000
// send FLUSH_WHEN_FULL packets when buffer is full
#define FLUSH_WHEN_FULL 800
#define LAG_DEFAULT 50

// don't need a chance
static Ihandle *inboundCheckbox, *outboundCheckbox, *timeInput;

static volatile short lagEnabled = 0,
    lagInbound = 1,
    lagOutbound = 1,
    lagTime = LAG_DEFAULT; // default for 50ms
static volatile short lagRangeEnabled = 0,
    lagTimeMin = LAG_DEFAULT,
    lagTimeMax = LAG_DEFAULT;
static short lagScenarioMinConfigured = 0,
    lagScenarioMaxConfigured = 0;

static PacketNode lagHeadNode = {0}, lagTailNode = {0};
static PacketNode *bufHead = &lagHeadNode, *bufTail = &lagTailNode;
static int bufSize = 0;

static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

static short clampLagValue(int value) {
    if (value < atoi(LAG_MIN)) {
        return atoi(LAG_MIN);
    }
    if (value > atoi(LAG_MAX)) {
        return atoi(LAG_MAX);
    }
    return (short)value;
}

static short chooseLagDelay() {
    short minDelay = lagTimeMin;
    short maxDelay = lagTimeMax;

    if (!lagRangeEnabled) {
        return lagTime;
    }
    if (maxDelay < minDelay) {
        short tmp = minDelay;
        minDelay = maxDelay;
        maxDelay = tmp;
    }
    if (minDelay == maxDelay) {
        return minDelay;
    }
    return (short)(minDelay + (rand() % (maxDelay - minDelay + 1)));
}

static Ihandle *lagSetupUI() {
    Ihandle *lagControlsBox = IupHbox(
        inboundCheckbox = IupToggle("Inbound", NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Delay(ms):"),
        timeInput = IupText(NULL),
        NULL
        );

    IupSetAttribute(timeInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(timeInput, "VALUE", STR(LAG_DEFAULT));
    IupSetCallback(timeInput, "VALUECHANGED_CB", uiSyncInteger);
    IupSetAttribute(timeInput, SYNCED_VALUE, (char*)&lagTime);
    IupSetAttribute(timeInput, INTEGER_MAX, LAG_MAX);
    IupSetAttribute(timeInput, INTEGER_MIN, LAG_MIN);
    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&lagInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&lagOutbound);

    // enable by default to avoid confusing
    IupSetAttribute(inboundCheckbox, "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");
    registerUiBinding(NAME"-inbound", inboundCheckbox, "VALUE", "ON");
    registerUiBinding(NAME"-outbound", outboundCheckbox, "VALUE", "ON");
    registerUiBinding(NAME"-time", timeInput, "VALUE", STR(LAG_DEFAULT));

    if (parameterized) {
        setFromParameter(inboundCheckbox, "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(timeInput, "VALUE", NAME"-time");
    }

    return lagControlsBox;
}

void resetLagScenarioConfig() {
    lagScenarioMinConfigured = 0;
    lagScenarioMaxConfigured = 0;
    InterlockedExchange16(&lagRangeEnabled, 0);
    InterlockedExchange16(&lagTimeMin, lagTime);
    InterlockedExchange16(&lagTimeMax, lagTime);
}

BOOL applyLagScenarioOption(const char *key, const char *value) {
    int parsedValue;
    short clampedValue;

    if (key == NULL || value == NULL) {
        return FALSE;
    }
    parsedValue = atoi(value);
    clampedValue = clampLagValue(parsedValue);

    if (_stricmp(key, NAME"-time") == 0) {
        InterlockedExchange16(&lagTime, clampedValue);
        return TRUE;
    }
    if (_stricmp(key, NAME"-time-min") == 0) {
        lagScenarioMinConfigured = 1;
        InterlockedExchange16(&lagTimeMin, clampedValue);
        return TRUE;
    }
    if (_stricmp(key, NAME"-time-max") == 0) {
        lagScenarioMaxConfigured = 1;
        InterlockedExchange16(&lagTimeMax, clampedValue);
        return TRUE;
    }

    return FALSE;
}

void finalizeLagScenarioConfig() {
    short minDelay = lagTimeMin;
    short maxDelay = lagTimeMax;

    if (lagScenarioMinConfigured && lagScenarioMaxConfigured) {
        if (maxDelay < minDelay) {
            short tmp = minDelay;
            minDelay = maxDelay;
            maxDelay = tmp;
        }
        InterlockedExchange16(&lagTimeMin, minDelay);
        InterlockedExchange16(&lagTimeMax, maxDelay);
        InterlockedExchange16(&lagRangeEnabled, 1);
    } else {
        InterlockedExchange16(&lagRangeEnabled, 0);
        InterlockedExchange16(&lagTimeMin, lagTime);
        InterlockedExchange16(&lagTimeMax, lagTime);
    }
}

static void lagStartUp() {
    if (bufHead->next == NULL && bufTail->next == NULL) {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize = 0;
    } else {
        assert(isBufEmpty());
    }
    startTimePeriod();
}

static void lagCloseDown(PacketNode *head, PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    UNREFERENCED_PARAMETER(head);
    // flush all buffered packets
    FORWARD_LOG("Closing down lag, flushing %d packets", bufSize);
    while(!isBufEmpty()) {
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    endTimePeriod();
}

static short lagProcess(PacketNode *head, PacketNode *tail) {
    DWORD currentTime = timeGetTime();
    PacketNode *pac = tail->prev;
    // pick up all packets and fill in the current time
    while (bufSize < KEEP_AT_MOST && pac != head) {
        if (checkDirection(pac->addr.Outbound, lagInbound, lagOutbound)) {
            PacketNode *lagged = insertAfter(popNode(pac), bufHead);
            lagged->timestamp = currentTime + chooseLagDelay();
            ++bufSize;
            pac = tail->prev;
        } else {
            pac = pac->prev;
        }
    }

    // try sending overdue packets from buffer tail
    while (!isBufEmpty()) {
        pac = bufTail->prev;
        if (currentTime >= pac->timestamp) {
            insertAfter(popNode(bufTail->prev), head); // sending queue is already empty by now
            --bufSize;
            FORWARD_LOG("Send lagged packets.");
        } else {
            FORWARD_LOG("Sent some lagged packets, still have %d in buf", bufSize);
            break;
        }
    }

    // if buffer is full just flush things out
    if (bufSize >= KEEP_AT_MOST) {
        int flushCnt = FLUSH_WHEN_FULL;
        while (flushCnt-- > 0) {
            insertAfter(popNode(bufTail->prev), head);
            --bufSize;
        }
    }

    return bufSize > 0;
}

Module lagModule = {
    "Lag",
    NAME,
    (short*)&lagEnabled,
    lagSetupUI,
    lagStartUp,
    lagCloseDown,
    lagProcess,
    // runtime fields
    0, 0, NULL
};
