#include <stdlib.h>
#include <string.h>
#include "common.h"

#define PACKET_POOL_BUCKET_MIN_SHIFT 6
#define PACKET_POOL_BUCKET_MAX_SHIFT 16
#define PACKET_POOL_BUCKET_COUNT (PACKET_POOL_BUCKET_MAX_SHIFT - PACKET_POOL_BUCKET_MIN_SHIFT + 1)
#define PACKET_POOL_NO_BUCKET ((unsigned short)PACKET_POOL_BUCKET_COUNT)

static PacketNode headNode = {0}, tailNode = {0};
PacketNode * const head = &headNode, * const tail = &tailNode;
static PacketNode *packetPool[PACKET_POOL_BUCKET_COUNT] = {0};

static INLINE_FUNCTION UINT packetBucketCapacity(unsigned short bucket) {
    return (UINT)(1u << (PACKET_POOL_BUCKET_MIN_SHIFT + bucket));
}

static UINT packetCapacityForLength(UINT len, unsigned short *bucket) {
    unsigned short ix = 0;
    UINT capacity = packetBucketCapacity(0);

    while (ix + 1 < PACKET_POOL_BUCKET_COUNT && capacity < len) {
        ++ix;
        capacity <<= 1;
    }

    if (capacity < len) {
        *bucket = PACKET_POOL_NO_BUCKET;
        return len;
    }

    *bucket = ix;
    return capacity;
}

void initPacketNodeList() {
    if (head->next == NULL && tail->prev == NULL) {
        // first time initializing
        head->next = tail;
        tail->prev = head;
    } else {
        // have used before, then check node is empty
        assert(isListEmpty());
    }
}

PacketNode* createNode(char* buf, UINT len, WINDIVERT_ADDRESS *addr) {
    PacketNode *newNode;
    unsigned short bucket;
    UINT capacity = packetCapacityForLength(len, &bucket);

    if (bucket != PACKET_POOL_NO_BUCKET && packetPool[bucket] != NULL) {
        newNode = packetPool[bucket];
        packetPool[bucket] = newNode->poolNext;
    } else {
        newNode = (PacketNode*)malloc(sizeof(PacketNode) + capacity);
        assert(newNode != NULL);
        newNode->packet = (char*)(newNode + 1);
        newNode->packetCapacity = capacity;
        newNode->packetBucket = bucket;
    }

    memcpy(newNode->packet, buf, len);
    newNode->packetLen = len;
    memcpy(&(newNode->addr), addr, sizeof(WINDIVERT_ADDRESS));
    newNode->next = newNode->prev = NULL;
    newNode->poolNext = NULL;
    return newNode;
}

void freeNode(PacketNode *node) {
    assert((node != head) && (node != tail));
    if (node->packetBucket != PACKET_POOL_NO_BUCKET) {
        node->poolNext = packetPool[node->packetBucket];
        packetPool[node->packetBucket] = node;
    } else {
        free(node);
    }
}

PacketNode* popNode(PacketNode *node) {
    assert((node != head) && (node != tail));
    node->prev->next = node->next;
    node->next->prev = node->prev;
    return node;
}

PacketNode* insertAfter(PacketNode *node, PacketNode *target) {
    assert(node && node != head && node != tail && target != tail);
    node->prev = target;
    node->next = target->next;
    target->next->prev = node;
    target->next = node;
    return node;
}

PacketNode* insertBefore(PacketNode *node, PacketNode *target) {
    assert(node && node != head && node != tail && target != head);
    node->next = target;
    node->prev = target->prev;
    target->prev->next = node;
    target->prev = node;
    return node;
}

PacketNode* appendNode(PacketNode *node) {
    return insertBefore(node, tail);
}

short isListEmpty() {
    return head->next == tail;
}
