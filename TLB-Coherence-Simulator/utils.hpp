//
//  utils.hpp
//  TLB-Coherence-Simulator
//
//  Created by Yashwant Marathe on 12/6/17.
//  Copyright © 2017 Yashwant Marathe. All rights reserved.
//

#ifndef utils_hpp
#define utils_hpp

#include <iostream>

//Defines
#define ADDR_SIZE 48

unsigned int log2(unsigned int num);

typedef enum {
    REQUEST_HIT,
    REQUEST_MISS,
    REQUEST_RETRY,
    MSHR_HIT,
    MSHR_HIT_AND_LOCKED,
} RequestStatus;

typedef enum {
    MODIFIED,
    OWNER,
    EXCLUSIVE,
    SHARED,
    INVALID,
} CoherenceState;

typedef enum {
    NONE,
    BROADCAST_DATA_READ,
    BROADCAST_DATA_WRITE,
    BROADCAST_TRANSLATION_READ,
    BROADCAST_TRANSLATION_WRITE,
    MEMORY_DATA_WRITEBACK,
    MEMORY_TRANSLATION_WRITEBACK,
} CoherenceAction;

typedef enum {
    DATA_READ,
    DATA_WRITE,
    TRANSLATION_READ,
    TRANSLATION_WRITE,
    DATA_WRITEBACK,
    TRANSLATION_WRITEBACK,
    BROADCASTED_DATA_WRITE,
    BROADCASTED_DATA_READ,
    BROADCASTED_TRANSLATION_WRITE,
    BROADCASTED_TRANSLATION_READ,
} kind;

#endif /* utils_hpp */
