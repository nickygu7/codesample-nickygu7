/**
 * @file csim.c
 * @author Yifan Gu
 * @brief Cache simulator
 *
 * This is an implementation of a cache simulator with command-line tool.
 * Follows LRU replacement policy when choosing which line to evict.
 * Follows a write-back, write allocate policy.
 * LRU is implemented using the "lastUsed" field.
 * When a line is used, its "lastUsed" is set to 0, all others in the set
 * are incremented by 1. The one being evicted is the one with the largest
 * "lastUsed".
 *
 * Command-line usage:
 *   ./csim [-v] -s <s> -E <E> -b <b> -t <trace>
 *   ./csim -h
 *
 * -h    Print this help message and exit
 * -v    Verbose mode: report effects of each memory operation
 * -s    <s> Number of set index bits (there are 2**s sets)
 * -b    <b> Number of block bits (there are 2**b blocks)
 * -E    <E> Number of lines per set (associativity)
 * -t    <trace> File name of the memory trace to process
 *
 * Trace files can be found in the traces/csim/ subdirectory.
 * Each line in the trace file must be in the format: Op Addr,Size.
 *
 * Op: denotes the type of memory access. It can be either L for a load,
 *     or S for a store.
 * Addr: gives the memory address to be accessed. It should be a 64-bit
 *       hexadecimal number, without a leading 0x.
 * Size: gives the number of bytes to be accessed at Addr. It should be
 *       a small, positive decimal number.
 *
 * @version 0.1
 * @date 2022-02-21
 *
 * @copyright Copyright (c) 2022
 */

#include "cache.h"
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int valid;           // 1 if the line is being used
    long tag;            // Used to match line
    unsigned long dirty; // 1 if the block is modified but not yet written back
    long lastUsed;       // Used to implement LRU replacement policy
} cacheLine;

// Globals set by command line args
int s = -1;               // Set index
int E = -1;               // Number of lines per set
int b = -1;               // Offset
bool verbose = false;     // Print trace if true
FILE *traceFile = NULL;
cacheLine **cache = NULL;
csim_stats_t stats;       // Output stats

/**
 * @brief Print help message when -h option is called or param error.
 *
 */
void printHelpMessage() {
    printf("Usage: ./csim [-v] -s <s> -E <E> -b <b> -t <trace>\n");
    printf("       ./csim -h\n\n");
    printf("  -h            Print this help message and exit\n");
    printf("  -v            Verbose mode: report effects of each memory\n");
    printf("  -s <s>        Number of set index bits (there are 2**s sets)\n");
    printf("  -b <b>        Number of block bits (there are 2**b blocks)\n");
    printf("  -E <E>        Number of lines per set (associativity)\n");
    printf("  -t <trace>    File name of the memory trace to process\n");
}

/**
 * @brief Parse input from command-line.
 *
 * @param argc The number of commmand-line arguments.
 * @param argv Array of character pointers listing all the arguments.
 */
void parseArgument(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "hvs:E:b:t:")) != -1) {
        switch (opt) {
        case 's':
            s = atoi(optarg);
            break;
        case 'E':
            E = atoi(optarg);
            break;
        case 'b':
            b = atoi(optarg);
            break;
        case 't':
            traceFile = fopen(optarg, "r");
            if (traceFile == NULL) {
                printf("File opening error.\n");
                exit(1);
            }
            break;
        case 'h':
            printHelpMessage();
            exit(0);
        case 'v':
            verbose = true;
            break;
        default:
            printf("Invalid input.\n");
            printHelpMessage();
            exit(1);
        }
    }

    // The value for -s, -b, -E, or -t is missing or
    // Not all of -s, -b, -E, and -t were supplied or
    // The value for -s, -b, or -E is not a positive integer, or is too large to
    // make sense.
    if (s < 0 || E <= 0 || b < 0 || s + b > 64) {
        printf("Invalid input.\n");
        printHelpMessage();
        exit(1);
    }

    return;
}

/**
 * @brief Initialize the cache.
 */
void init() {
    int S = (int)pow(2, s);
    cache = (cacheLine **)malloc(sizeof(cacheLine *) * (unsigned long)S);
    if (cache == NULL) {
        printf("Invalid set memory\n");
        exit(1);
    }

    for (int i = 0; i < S; i++) {
        cache[i] = (cacheLine *)malloc(sizeof(cacheLine) * (unsigned long)E);
        if (cache[i] == NULL) {
            printf("Invalid line memory\n");
            exit(1);
        }
        // Set all fields to 0
        for (int j = 0; j < E; j++) {
            cache[i][j].valid = 0;
            cache[i][j].lastUsed = 0;
            cache[i][j].dirty = 0;
        }
    }
    return;
}

/**
 * @brief Update "lastUsed" field of the cache line.
 *
 * @param set Set index of the cache line being updated.
 * @param offset Block offset of the cache line being updated.
 */
void updateLastUsed(long set, long offset) {
    cache[set][offset].lastUsed = 0;
    // Fields "lastUsed" in all the other lines are incremented by 1
    for (int i = 0; i < E; i++) {
        if (i != offset) {
            cache[set][i].lastUsed++;
        }
    }
}

/**
 * @brief Find the least recently used cache line using field "lastUsed".
 *
 * @param set Set index of the cache line being updated.
 * @return int
 */
int findLeastRecentlyUsed(long set) {
    int maxIndex = 0;
    for (int i = 0; i < E; i++) {
        if (cache[set][i].lastUsed > cache[set][maxIndex].lastUsed) {
            maxIndex = i;
        }
    }
    return maxIndex;
}

/**
 * @brief Judge if the operation results in cache miss.
 *
 * @param set Set index of the target cache line.
 * @param tag Used to match cache line.
 * @param operation Denotes the type of memory access.
 * @return true if it causes a cache miss.
 * @return false if it causes a cache hit.
 */
bool isMiss(long set, long tag, int operation) {
    bool isMiss = true;
    for (int i = 0; i < E; i++) {
        // If it's a cache hit, update "lastUsed"
        if (cache[set][i].valid == 1 && cache[set][i].tag == tag) {
            isMiss = false;
            updateLastUsed(set, i);

            if (operation == 'S' && cache[set][i].dirty == 0) {
                cache[set][i].dirty = 1;
                stats.dirty_bytes += (unsigned long)pow(2, b);
            }
            break;
        }
    }
    return isMiss;
}

/**
 * @brief Load bytes from memory to cache
 *
 * @param set Set index of the target cache line.
 * @param tag Used to match cache line.
 * @param operation Denotes the type of memory access.
 * @return true if the set is full.
 * @return false if the set is not full.
 */
bool updateCache(long set, long tag, char operation) {
    bool isFull = true;
    int i;
    for (i = 0; i < E; i++) {
        if (cache[set][i].valid == 0) {
            isFull = false;
            cache[set][i].valid = 1;
            cache[set][i].tag = tag;
            cache[set][i].dirty = 0;
            updateLastUsed(set, i);
            break;
        }
    }

    // Index of the cache line being updated
    int index = i;

    // Evict the line with largest "lastUsed"
    if (isFull) {
        int evicted = findLeastRecentlyUsed(set);
        cache[set][evicted].valid = 1;
        cache[set][evicted].tag = tag;
        if (cache[set][evicted].dirty) {
            stats.dirty_evictions += (unsigned long)pow(2, b);
            stats.dirty_bytes -= (unsigned long)pow(2, b);
            cache[set][evicted].dirty = 0;
        }
        updateLastUsed(set, evicted);
        index = evicted;
    }

    if (operation == 'S') {
        cache[set][index].dirty = 1;
        stats.dirty_bytes += (unsigned long)pow(2, b);
    }

    return isFull;
}

/**
 * @brief Load or save data operation read from the trace file.
 *
 * @param addr Gives the memory address to be accessed.
 * @param operation Denotes the type of memory access.
 */
void updateData(long addr, char operation) {
    long set = addr >> b & ((1 << s) - 1);
    long tag = addr >> (s + b);

    if (isMiss(set, tag, operation)) {
        stats.misses++;
        if (verbose) {
            printf("miss ");
        }

        if (updateCache(set, tag, operation)) {
            stats.evictions++;
            if (verbose) {
                printf("eviction");
            }
        }
    } else {
        stats.hits++;
        if (verbose) {
            printf("hit");
        }
    }
    return;
}

int main(int argc, char *argv[]) {
    parseArgument(argc, argv);
    init();
    char buf[20];
    char operation;
    long addr;
    int size;

    // Read trace file and parse line by line
    while (fgets(buf, sizeof(buf), traceFile) != NULL) {
        const char *delim = " ,";
        operation = buf[0];
        strtok(buf, delim);
        addr = strtol(strtok(NULL, delim), NULL, 16);
        size = atoi(strtok(NULL, delim));
        if (verbose) {
            printf("%c %lx,%d ", operation, addr, size);
        }
        if (operation == 'L' | operation == 'S') {
            updateData(addr, operation);
        }
        if (verbose) {
            printf("\n");
        }
    }

    printSummary(&stats);
    return 0;
}
