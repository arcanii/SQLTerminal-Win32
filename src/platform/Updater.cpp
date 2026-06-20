// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Updater.h"

#include <winsparkle.h>

namespace sqlterm {

void initUpdater() {
    // Windows appcast (separate from the macOS feed). Replace with the published
    // URL for releases; until one exists, checks simply find nothing.
    win_sparkle_set_appcast_url(
        "https://raw.githubusercontent.com/arcanii/SQLTerminal-Win32/main/appcast.xml");
    win_sparkle_set_app_details(L"SQLTerminal", L"SQLTerminal", L"0.1.0");
    // EdDSA public key (same scheme as the macOS SUPublicEDKey). Windows update
    // packages must be signed with the matching private key.
    win_sparkle_set_eddsa_public_key("sKPprIa95Hw+DX3bMoxWMsyC0w9vc4MzEpgx7TBDP1I=");
    win_sparkle_init();
}

void checkForUpdates() { win_sparkle_check_update_with_ui(); }

void shutdownUpdater() { win_sparkle_cleanup(); }

}  // namespace sqlterm
