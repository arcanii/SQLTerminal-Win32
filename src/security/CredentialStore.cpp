// SPDX-License-Identifier: GPL-3.0-or-later
#include "security/CredentialStore.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

#include "platform/Encoding.h"

namespace sqlterm {

namespace {
std::wstring targetName(const std::wstring& account) { return L"SQLTerminal:" + account; }
}  // namespace

std::wstring CredentialStore::accountKey(const DatabaseConnection& c) {
    return c.username + L"@" + c.host + L":" + c.port + L"/" + c.databaseName;
}

bool CredentialStore::savePassword(const std::wstring& account, const std::wstring& password) {
    const std::wstring target = targetName(account);
    std::string blob = utf8FromWide(password);  // store UTF-8 bytes, like the Swift app

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = blob.empty() ? nullptr
                                       : reinterpret_cast<LPBYTE>(blob.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = const_cast<LPWSTR>(account.c_str());
    return CredWriteW(&cred, 0) == TRUE;
}

std::optional<std::wstring> CredentialStore::password(const std::wstring& account) {
    const std::wstring target = targetName(account);
    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pcred)) return std::nullopt;
    std::wstring result = wideFromUtf8(reinterpret_cast<const char*>(pcred->CredentialBlob),
                                       static_cast<int>(pcred->CredentialBlobSize));
    CredFree(pcred);
    return result;
}

bool CredentialStore::deletePassword(const std::wstring& account) {
    const std::wstring target = targetName(account);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

}  // namespace sqlterm
