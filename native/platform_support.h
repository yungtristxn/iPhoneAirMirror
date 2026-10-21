#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

struct LanCandidate {
    std::array<uint8_t,4> address{};
    uint32_t index=0,prefix=0;
    bool up=false,ethernet_or_wifi=false,gateway=false,private_profile=false;
};
static bool select_lan_candidate(const std::vector<LanCandidate>& candidates,
                                 const uint8_t* requested,LanCandidate& result) {
    for(const auto& candidate:candidates){
        const auto& b=candidate.address;
        const bool private_address=b[0]==10||(b[0]==172&&b[1]>=16&&b[1]<=31)||(b[0]==192&&b[1]==168);
        if(!candidate.up||!candidate.ethernet_or_wifi||!candidate.gateway||!candidate.private_profile
            ||!candidate.index||candidate.prefix<1||candidate.prefix>32||!private_address)continue;
        if(requested&&!std::equal(b.begin(),b.end(),requested))continue;
        result=candidate;return true;
    }
    return false;
}

// This preflight recognizes an existing grant for the specific worker identity.
// CreateProcess still performs the complete Windows access check, including
// parent-directory access and other token groups. Never weaken a denied launch.
static bool worker_acl_grants_execute(PACL acl,PSID sid) {
    if(!acl)return true; // A null DACL already grants access; no mutation is needed.
    if(!IsValidAcl(acl)||!IsValidSid(sid))return false;
    const DWORD required=FILE_GENERIC_READ|FILE_GENERIC_EXECUTE;
    DWORD allowed=0;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
    for(DWORD i=0;i<acl->AceCount;++i){
        void* entry=nullptr;if(!GetAce(acl,i,&entry))return false;
        auto header=static_cast<ACE_HEADER*>(entry);
        if(header->AceFlags&INHERIT_ONLY_ACE)continue;
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE&&header->AceType!=ACCESS_DENIED_ACE_TYPE)continue;
        auto ace=static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if(header->AceSize<offsetof(ACCESS_ALLOWED_ACE,SidStart)+8)return false;
        PSID entry_sid=&ace->SidStart;
        if(GetSidLengthRequired(*GetSidSubAuthorityCount(entry_sid))>header->AceSize- offsetof(ACCESS_ALLOWED_ACE,SidStart)
            ||!IsValidSid(entry_sid))return false;
        if(!EqualSid(entry_sid,sid))continue;
        DWORD mask=ace->Mask;MapGenericMask(&mask,&mapping);
        if(header->AceType==ACCESS_DENIED_ACE_TYPE&&(mask&required))return false;
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE)allowed|=mask;
    }
    return (allowed&required)==required;
}

static HRESULT worker_profile_sid(PSID* sid) {
    HRESULT hr=CreateAppContainerProfile(L"AirMirrorLite.Codec",L"AirMirrorLite codec",L"No network capabilities",nullptr,0,sid);
    if(hr==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))hr=DeriveAppContainerSidFromAppContainerName(L"AirMirrorLite.Codec",sid);
    return hr;
}
static HRESULT current_executable(std::wstring& path) {
    path.resize(32768);DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!length)return HRESULT_FROM_WIN32(GetLastError());
    if(length>=path.size())return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    path.resize(length);return S_OK;
}
static HRESULT worker_executable_access(std::wstring& path,PSID sid,bool provision) {
    PACL old_acl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
    DWORD rc=GetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&old_acl,nullptr,&descriptor);
    if(rc!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(rc);
    struct DescriptorGuard { PSECURITY_DESCRIPTOR value;~DescriptorGuard(){LocalFree(value);} } guard{descriptor};
    if(worker_acl_grants_execute(old_acl,sid))return S_OK;
    if(!provision)return E_ACCESSDENIED;
    EXPLICIT_ACCESSW access{};access.grfAccessPermissions=FILE_GENERIC_READ|FILE_GENERIC_EXECUTE;
    access.grfAccessMode=GRANT_ACCESS;access.Trustee.TrusteeForm=TRUSTEE_IS_SID;access.Trustee.ptstrName=static_cast<LPWSTR>(sid);
    PACL updated=nullptr;rc=SetEntriesInAclW(1,&access,old_acl,&updated);
    if(rc==ERROR_SUCCESS){
        // Retained deny ACEs must not be bypassed by adding another allow ACE.
        if(!worker_acl_grants_execute(updated,sid))rc=ERROR_ACCESS_DENIED;
        else rc=SetNamedSecurityInfoW(path.data(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,updated,nullptr);
    }
    if(updated)LocalFree(updated);
    return HRESULT_FROM_WIN32(rc);
}

static HRESULT probe_platform_contracts() {
    std::vector<LanCandidate> candidates{
        {{{192,168,1,2}},7,24,true,true,true,true},
        {{{10,2,3,4}},9,16,true,true,true,true},
        {{{8,8,8,8}},11,24,true,true,true,true}};
    LanCandidate chosen;const uint8_t second[]={10,2,3,4},public_ip[]={8,8,8,8};
    if(!select_lan_candidate(candidates,nullptr,chosen)||chosen.index!=7)return E_FAIL;
    if(!select_lan_candidate(candidates,second,chosen)||chosen.index!=9||chosen.prefix!=16)return E_FAIL;
    if(select_lan_candidate(candidates,public_ip,chosen))return E_FAIL;
    candidates[1].private_profile=false;if(select_lan_candidate(candidates,second,chosen))return E_FAIL;
    candidates[1].private_profile=true;candidates[1].gateway=false;
    if(select_lan_candidate(candidates,second,chosen))return E_FAIL;
    PSID sid=nullptr;HRESULT hr=DeriveAppContainerSidFromAppContainerName(L"AirMirrorLite.Codec",&sid);
    if(FAILED(hr))return hr;
    struct SidGuard { PSID value;~SidGuard(){FreeSid(value);} } sid_guard{sid};
    EXPLICIT_ACCESSW grant{};grant.grfAccessPermissions=GENERIC_READ|GENERIC_EXECUTE;grant.grfAccessMode=GRANT_ACCESS;
    grant.Trustee.TrusteeForm=TRUSTEE_IS_SID;grant.Trustee.ptstrName=static_cast<LPWSTR>(sid);
    PACL acl=nullptr;DWORD rc=SetEntriesInAclW(1,&grant,nullptr,&acl);
    if(rc!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(rc);
    struct AclGuard { PACL value;~AclGuard(){LocalFree(value);} } acl_guard{acl};
    std::vector<uint8_t> before(reinterpret_cast<uint8_t*>(acl),reinterpret_cast<uint8_t*>(acl)+acl->AclSize);
    if(!worker_acl_grants_execute(acl,sid)||std::memcmp(before.data(),acl,before.size()))return E_FAIL;
    grant.grfAccessPermissions=FILE_EXECUTE;grant.grfAccessMode=DENY_ACCESS;PACL denied=nullptr;
    rc=SetEntriesInAclW(1,&grant,acl,&denied);if(rc!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(rc);
    AclGuard denied_guard{denied};if(worker_acl_grants_execute(denied,sid))return E_FAIL;
    return S_OK;
}
