/*
 * Crux2 - the notification relay: a tiny payload that posts one PS5 notification.
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 * From Porpoise 1679c95 (tools/notify-relay/notify_relay.c, GPL-3.0-or-later), whose approach and
 * findings (only notifications kept in the list show, the 3 s wait, _exit) are PS5SX2's
 * (ps5/coreorbis/notify-relay/notify_relay.c, GPL-3.0-or-later).
 *
 * A title may be refused libSceNotification on recent firmware, so the trophy-style notifications
 * of RetroAchievements' unlocks go through the payload loader instead: Crux2
 * (frontend/ps5_notify.cpp) writes the notification's JSON over g_payload's marker in a copy of
 * this program (notify-relay.elf in the title folder, built by ps5/tools/build-notify-relay.sh)
 * and sends it to 127.0.0.1:9021, where the loader runs it as a process of its own.
 */

#include <stdbool.h>
#include <unistd.h>

int sceNotificationSend(int userId, bool isLogged, const char *payload);

/* Initialized, so the whole array is in the file: the room Crux2 fills (kRelayRoom in
 * frontend/ps5_notify.cpp). volatile: what Crux2 wrote is read, not the marker seen here. */
__attribute__((used)) volatile char g_payload[16384] = "CRUX2-NOTIFY-RELAY-JSON-V1";

int main(void)
{
    if (g_payload[0] != '{')
        return 1;
    /* 0xFE: the system user. Only a notification kept in the list shows. */
    const int rc = sceNotificationSend(0xFE, true, (const char *)g_payload);
    /* The library posts from a thread of its own: give it time, then end without unloading
     * anything under it. */
    sleep(3);
    _exit(rc == 0 ? 0 : 2);
}
