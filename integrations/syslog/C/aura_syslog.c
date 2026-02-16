/**
 * @file aura_syslog.c
 * @brief Syslog log handler integration for AuraIO
 */

#include "aura_syslog.h"

#include <syslog.h>

/**
 * Log callback — forwards to syslog(3).
 *
 * AuraIO log levels match syslog priorities 1:1, so we pass the level
 * through directly.  The "%s" format prevents format-string injection
 * if the message happens to contain percent characters.
 */
static void syslog_handler(int level, const char *msg, void *userdata) {
    (void)userdata;
    syslog(level, "%s", msg);
}

void aura_syslog_install(const aura_syslog_options_t *options) {
    const char *ident = "aura";
    int facility = LOG_USER;
    int log_options = LOG_PID | LOG_NDELAY;

    if (options) {
        if (options->ident) ident = options->ident;
        if (options->facility >= 0) facility = options->facility;
        if (options->log_options >= 0) log_options = options->log_options;
    }

    openlog(ident, log_options, facility);
    aura_set_log_handler(syslog_handler, NULL);
}

void aura_syslog_remove(void) {
    aura_set_log_handler(NULL, NULL);
    closelog();
}
