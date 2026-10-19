//
//  Logger.m
//  Moonlight
//
//  Created by Diego Waxemberg on 2/10/15.
//  Copyright (c) 2015 Moonlight Stream. All rights reserved.
//

#import "Logger.h"

static LogLevel LoggerLogLevel = LOG_I;

#if TARGET_OS_TV
#include <sys/time.h>

// FrogtaleTV patch: the log only goes to the console, which is gone unless Xcode is attached, so a
// Starlink test session couldn't be read afterwards. Also write it (ours and moonlight-common-c's)
// to Library/Caches/Moonlight.log, replaced on each launch. Download it with Xcode -> Devices and
// Simulators -> the Apple TV -> Moonlight Starlink -> Download Container.
static FILE* logFile;

void LogFileWrite(NSString* line) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString* cacheDir = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
        logFile = fopen([[cacheDir stringByAppendingPathComponent:@"Moonlight.log"] fileSystemRepresentation], "w");
        if (logFile != NULL) {
            setvbuf(logFile, NULL, _IOLBF, 0);
        }
    });
    
    const char* text = line.UTF8String;
    if (logFile == NULL || text == NULL) {
        return;
    }
    
    // Wall clock time, so stalls can be lined up with Starlink's :12/:27/:42/:57 handoffs
    // (NSCalendar rather than localtime_r: Xcode 27's explicit modules reject struct tm here)
    NSDate* now = [NSDate date];
    NSDateComponents* c = [[NSCalendar currentCalendar] components:NSCalendarUnitHour | NSCalendarUnitMinute |
                                                                   NSCalendarUnitSecond | NSCalendarUnitNanosecond
                                                          fromDate:now];
    size_t length = strlen(text);
    fprintf(logFile, "%02ld:%02ld:%02ld.%03d %s%s", (long)c.hour, (long)c.minute, (long)c.second,
            (int)(c.nanosecond / 1000000), text, (length > 0 && text[length - 1] == '\n') ? "" : "\n");
}
#endif

void LogTagv(LogLevel level, NSString* tag, NSString* fmt, va_list args);

void Log(LogLevel level, NSString* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogTagv(level, NULL, fmt, args);
    va_end(args);
}

void LogTag(LogLevel level, NSString* tag, NSString* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogTagv(level, tag, fmt, args);
    va_end(args);
}

void LogTagv(LogLevel level, NSString* tag, NSString* fmt, va_list args) {
    NSString* levelPrefix = @"";
    
    if (level < LoggerLogLevel) {
        return;
    }
    
    switch(level) {
        case LOG_D:
            levelPrefix = PRFX_DEBUG;
            break;
        case LOG_I:
            levelPrefix = PRFX_INFO;
            break;
        case LOG_W:
            levelPrefix = PRFX_WARN;
            break;
        case LOG_E:
            levelPrefix = PRFX_ERROR;
            break;
        default:
            levelPrefix = @"";
            assert(false);
            break;
    }
    NSString* prefixedString;
    if (tag) {
        prefixedString = [NSString stringWithFormat:@"%@ (%@) %@", levelPrefix, tag, fmt];
    } else {
        prefixedString = [NSString stringWithFormat:@"%@ %@", levelPrefix, fmt];
    }
#if TARGET_OS_TV
    va_list fileArgs;
    va_copy(fileArgs, args);
    NSLogv(prefixedString, args);
    LogFileWrite([[NSString alloc] initWithFormat:prefixedString arguments:fileArgs]);
    va_end(fileArgs);
#else
    NSLogv(prefixedString, args);
#endif
}
