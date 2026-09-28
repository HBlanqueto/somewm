#ifndef IPC_H
#define IPC_H

#include <stdbool.h>
#include <wayland-server-core.h>

/**
 * Initialize IPC socket and integrate with Wayland event loop
 * Creates a Unix domain socket at $XDG_RUNTIME_DIR/somewm-socket
 * and registers it with the event loop for non-blocking I/O.
 *
 * @param event_loop The Wayland event loop to integrate with
 * @return 0 on success, -1 on error
 */
int ipc_init(struct wl_event_loop *event_loop);

/**
 * Cleanup IPC resources
 * Closes all client connections and the listening socket.
 * Removes the socket file from the filesystem.
 */
void ipc_cleanup(void);

/**
 * Get the IPC socket path
 * Useful for error messages and debugging
 *
 * @return Path to the IPC socket file
 */
const char *ipc_get_socket_path(void);

/**
 * Send response to IPC client
 */
void ipc_send_response(int client_fd, const char *response);

/**
 * Mark a client as a subscriber for event broadcasts
 */
void ipc_subscribe_client(int client_fd);

/**
 * Is any connected client subscribed?
 */
bool ipc_has_subscribers(void);

/**
 * Broadcast an event message to all subscribed clients
 * Format: EVENT <type> <json>\n
 */
void ipc_broadcast(const char *message);

/* ---------------------------------------------------------------------------
 * Outside-press watcher (generic, armed on demand)
 *
 * A subscriber (the shell) asks the compositor to notify it when a button
 * press lands OUTSIDE a given layer-surface namespace, and can stop asking.
 * While nobody has asked, the compositor broadcasts nothing. The fork stores
 * only the watched namespace; it never learns what surface owns it. The press
 * is OBSERVED, never consumed: the compositor broadcasts and the press
 * continues to whatever it hit, unchanged.
 */

/**
 * Arm the watcher on a namespace. A press whose target surface's namespace
 * differs from `ns` (or that hits a client/drawin/empty desktop) broadcasts
 * EVENT outside_press. Arm/disarm are idempotent.
 */
void ipc_outside_press_arm(const char *ns);

/**
 * Disarm the watcher: no further presses broadcast anything.
 */
void ipc_outside_press_disarm(void);

/**
 * The currently watched namespace, or NULL while disarmed.
 */
const char *ipc_outside_press_namespace(void);

#endif /* IPC_H */
