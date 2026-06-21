// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Updater.h"

#include <winsparkle.h>

#include "version.h"

namespace sqlterm {

void initUpdater() {
    // Windows appcast hosted on GitHub, mirroring the macOS Sparkle feed (and using
    // the same EdDSA key pair). Releasing/signing is documented in docs/RELEASING.md.
    win_sparkle_set_appcast_url(
        "https://raw.githubusercontent.com/arcanii/SQLTerminal-Win32/main/appcast.xml");
    // Report marketing.build (e.g. 0.1.0.42) so each committed build compares
    // correctly against the appcast's sparkle:version.
    win_sparkle_set_app_details(L"SQLTerminal", L"SQLTerminal", SQLT_VERSION_FULL_W);
    // EdDSA public key (same scheme as the macOS SUPublicEDKey). Windows update
    // packages must be signed with the matching private key.
    win_sparkle_set_eddsa_public_key("sKPprIa95Hw+DX3bMoxWMsyC0w9vc4MzEpgx7TBDP1I=");
    win_sparkle_init();
}

void checkForUpdates() { win_sparkle_check_update_with_ui(); }

void shutdownUpdater() { win_sparkle_cleanup(); }

}  // namespace sqlterm
