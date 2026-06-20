// SPDX-License-Identifier: GPL-3.0-or-later
// Thin wrapper over WinSparkle (the Windows analogue of Sparkle). Auto-update
// checks run against a Windows appcast; packages must be EdDSA-signed with the
// key matching the public key set below.
#pragma once

namespace sqlterm {

void initUpdater();      // configure + start background checks
void checkForUpdates();  // user-triggered "Check for Updates…"
void shutdownUpdater();  // on app exit

}  // namespace sqlterm
