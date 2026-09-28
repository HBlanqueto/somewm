/*
 * banning.c - client banning management
 *
 * Copyright © 2007-2009 Julien Danjou <julien@danjou.info>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "banning.h"
#include "globalconf.h"
#include "slide.h"
#include "objects/client.h"
#include "somewm_api.h"

/** Reban windows following current selected tags.
 */
void
banning_need_update(void)
{
    /* We update the complete banning only once per main loop to avoid
     * excessive updates...  */
    globalconf.need_lazy_banning = true;

    /* But if a client will be banned in our next update we unfocus it now. */
    foreach(_c, globalconf.clients)
    {
        client_t *c = *_c;

        if(!client_isvisible(c))
            client_ban_unfocus(c);
    }
}

/** Check all clients if they need to rebanned
 */
void
banning_refresh(void)
{
    /* The tag-slide driver takes over a 1->1 tag switch (starting the slide
     * and deferring the real ban/unban to its end), and keeps the persistent
     * focus-space backdrop in sync. */
    if (slide_handle_banning())
        return;

    /* No slide took over the switch: a deferred reveal park release (from a
     * space that was left while the switch was pending) has nothing to ride,
     * so snap it now. An active slide owns the release and ends it at its
     * teardown, so a refresh cycle while the slide runs must not flush
     * mid-flight. */
    if (!slide_active())
        reveal_release_flush();

    /* Settle-time self-healing: whatever the mechanism was, when the banning
     * pass runs with no transition pending, no layer surface may hold a park
     * offset (an ordinary desktop cannot leave one behind). Fires only when
     * no slide/release is easing a descent, so it never snaps mid-slide. */
    reveal_release_stale();

    if (!globalconf.need_lazy_banning)
        return;

    globalconf.need_lazy_banning = false;

    foreach(c, globalconf.clients)
        if(client_isvisible(*c))
            client_unban(*c);

    /* Some people disliked the short flicker of background, so we first unban everything.
     * Afterwards we ban everything we don't want. This should avoid that. */
    foreach(c, globalconf.clients)
        if(!client_isvisible(*c))
            client_ban(*c);

    /* A tag switch without a slide: apply tag.layers visibility to layer
     * surfaces now that the client ban/unban settled and need_lazy_banning is
     * clear (the sync is deferred while a switch is pending so the slide can
     * still slide the outgoing surface out). */
    slide_layer_tag_visibility_sync_all();
}

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
