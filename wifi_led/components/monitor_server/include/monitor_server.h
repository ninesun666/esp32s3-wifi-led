#pragma once

/* Starts the HTTP server (port from Kconfig), publishes the board over mDNS, and
 * serves the monitor page plus /api/status and /api/led. */
void monitor_server_start(void);
