#include "Dialog.hpp"

#include "API/CAppManager.hpp"
#include "API/CServerExoApp.hpp"
#include "API/CNWSObject.hpp"
#include "API/CNWSDialog.hpp"
#include "API/CNWSDialogEntry.hpp"
#include "API/CNWSDialogReply.hpp"
#include "API/CNWSDialogLinkEntry.hpp"
#include "API/CNWSDialogLinkReply.hpp"
#include "API/CNWSDialogSpeaker.hpp"
#include "API/CExoLocString.hpp"
#include "API/CExoLinkedListInternal.hpp"
#include "API/CExoString.hpp"
#include "API/CResRef.hpp"
#include "API/Constants.hpp"
#include "API/Globals.hpp"
#include "API/Functions.hpp"
#include "Services/Events/Events.hpp"
#include "Services/Hooks/Hooks.hpp"
#include "Utils.hpp"

using namespace NWNXLib;
using namespace NWNXLib::API;

static Dialog::Dialog* g_plugin;

NWNX_PLUGIN_ENTRY Plugin::Info* PluginInfo()
{
    return new Plugin::Info
    {
        "Dialog",
        "Functions exposing additional dialog properties",
        "sherincall",
        "sherincall@gmail.com",
        1,
        true
    };
}

NWNX_PLUGIN_ENTRY Plugin* PluginLoad(Plugin::CreateParams params)
{
    g_plugin = new Dialog::Dialog(params);
    return g_plugin;
}

namespace Dialog {

//
// Constants mirrored from NSS
const int32_t NODE_TYPE_INVALID       = -1;
const int32_t NODE_TYPE_STARTING_NODE = 0;
const int32_t NODE_TYPE_ENTRY_NODE    = 1;
const int32_t NODE_TYPE_REPLY_NODE    = 2;

const int32_t SCRIPT_TYPE_OTHER = 0;
const int32_t SCRIPT_TYPE_STARTING_CONDITIONAL = 1;
const int32_t SCRIPT_TYPE_ACTION_TAKEN = 2;

//
// Hooks to maintain the state stack
//
Dialog::State Dialog::statestack[16];
int32_t Dialog::ssp;
CNWSDialog *Dialog::pDialog;
CNWSObject *Dialog::pOwner;

uint32_t Dialog::newSpeaker = Constants::OBJECT_INVALID;
NWNXLib::Hooking::FunctionHook* Dialog::m_GetSpeakerHook;

uint32_t Dialog::idxEntry;
uint32_t Dialog::idxReply;
int32_t  Dialog::scriptType;
int32_t  Dialog::loopCount;

void Dialog::Hooks::GetStartEntry(bool before, CNWSDialog *pThis,
    CNWSObject* pNWSObjectOwner)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    loopCount = 0;
    if (before)
        statestack[++ssp] = DIALOG_STATE_START;
    else ssp--;
}

void Dialog::Hooks::GetStartEntryOneLiner(bool before, CNWSDialog *pThis,
    CNWSObject* pNWSObjectOwner, CExoLocString* sOneLiner, CResRef* sSound, CResRef* sScript)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    loopCount = 0;
    (void)sOneLiner; (void)sSound; (void)sScript;
    if (before)
        statestack[++ssp] = DIALOG_STATE_START;
    else ssp--;
}

void Dialog::Hooks::SendDialogEntry(bool before, CNWSDialog *pThis,
    CNWSObject* pNWSObjectOwner, uint32_t nPlayerIdGUIOnly, uint32_t iEntry, int32_t bPlayHelloSound)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    loopCount = 0;
    (void)nPlayerIdGUIOnly; (void)bPlayHelloSound;
    if (before)
    {
        statestack[++ssp] = DIALOG_STATE_SEND_ENTRY;
        idxEntry = iEntry;
        if (g_plugin->newSpeaker != Constants::OBJECT_INVALID)
        {
            pDialog->m_pEntries[idxEntry].m_sSpeaker = Utils::AsNWSObject(Utils::GetGameObject(g_plugin->newSpeaker))->m_sTag;
            LOG_DEBUG("Speaker set to %s", pDialog->m_pEntries[idxEntry].m_sSpeaker);
            g_plugin->newSpeaker = Constants::OBJECT_INVALID;
        }
    }
    else ssp--;
}

void Dialog::Hooks::SendDialogReplies(bool before, CNWSDialog *pThis,
    CNWSObject* pNWSObjectOwner, uint32_t nPlayerIdGUIOnly)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    loopCount = 0;
    (void)nPlayerIdGUIOnly;
    if (before)
        statestack[++ssp] = DIALOG_STATE_SEND_REPLIES;
    else ssp--;
}

void Dialog::Hooks::HandleReply(bool before, CNWSDialog *pThis,
    uint32_t nPlayerID, CNWSObject* pNWSObjectOwner, uint32_t nReplyIndex, int32_t bEscapeDialog, uint32_t currentEntryIndex)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    loopCount = 0;
    (void)bEscapeDialog; (void)nPlayerID;
    if (before)
    {
        statestack[++ssp] = DIALOG_STATE_HANDLE_REPLY;
        idxEntry = currentEntryIndex;
        idxReply = nReplyIndex;
    }
    else ssp--;
}

void Dialog::Hooks::CheckScript(bool before, CNWSDialog *pThis,
    CNWSObject* pNWSObjectOwner, const CResRef* sActive)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    (void)sActive;
    if (before)
    {
        if (statestack[ssp] == DIALOG_STATE_HANDLE_REPLY)
        {
            statestack[ssp] = DIALOG_STATE_SEND_ENTRY;
            idxReply = pDialog->m_pEntries[idxEntry].m_pReplies[idxReply].m_nIndex;
            idxEntry = pDialog->m_pReplies[idxReply].m_pEntries[loopCount].m_nIndex;
        }
        scriptType = SCRIPT_TYPE_STARTING_CONDITIONAL;
    }
    else
    {
        loopCount++;
        scriptType = SCRIPT_TYPE_OTHER;
    }
}

void Dialog::Hooks::RunScript(bool before, CNWSDialog *pThis,
    CNWSObject* pNWSObjectOwner, const CResRef* sScript)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;
    (void)sScript;
    if (before)
        scriptType = SCRIPT_TYPE_ACTION_TAKEN;
    else
        scriptType = SCRIPT_TYPE_OTHER;
}

CNWSObject* Dialog::Hooks::GetSpeaker(CNWSDialog* pThis,
    CNWSObject* pNWSObjectOwner, const CExoString& sSpeaker)
{
    pDialog = pThis;
    pOwner = pNWSObjectOwner;

    if (auto* pObject = Utils::AsNWSObject(Utils::GetGameObject(g_plugin->newSpeaker)))
    {        
        g_plugin->newSpeaker = Constants::OBJECT_INVALID;
        return pObject;
    }
    return g_plugin->m_GetSpeakerHook->CallOriginal<CNWSObject*>(pNWSObjectOwner, sSpeaker);
}

Dialog::Dialog(const Plugin::CreateParams& params)
    : Plugin(params)
{
#define REGISTER(func) \
    GetServices()->m_events->RegisterEvent(#func, \
        [this](ArgumentStack&& args){ return func(std::move(args)); })

    REGISTER(GetCurrentNodeType);
    REGISTER(GetCurrentScriptType);
    REGISTER(GetCurrentNodeID);
    REGISTER(GetCurrentNodeIndex);
    REGISTER(GetCurrentNodeText);
    REGISTER(SetCurrentNodeText);
    REGISTER(End);
    REGISTER(SetNPCSpeaker);

#undef REGISTER

    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog13GetStartEntryEP10CNWSObject,
            uint32_t, CNWSDialog*, CNWSObject*>(&Hooks::GetStartEntry);
    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog21GetStartEntryOneLinerEP10CNWSObjectR13CExoLocStringR7CResRefS5_,
            int32_t, CNWSDialog*, CNWSObject*, CExoLocString*, CResRef*, CResRef*>(&Hooks::GetStartEntryOneLiner);
    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog15SendDialogEntryEP10CNWSObjectjji,
            int32_t, CNWSDialog*, CNWSObject*, uint32_t, uint32_t, int32_t>(&Hooks::SendDialogEntry);
    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog17SendDialogRepliesEP10CNWSObjectj,
            int32_t, CNWSDialog*, CNWSObject*, uint32_t>(&Hooks::SendDialogReplies);
    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog11HandleReplyEjP10CNWSObjectjij,
            int32_t, CNWSDialog*, uint32_t , CNWSObject*, uint32_t, int32_t, uint32_t>(&Hooks::HandleReply);
    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog11CheckScriptEP10CNWSObjectRK7CResRef,
            int32_t, CNWSDialog *, CNWSObject*, const CResRef*>(&Hooks::CheckScript);
    GetServices()->m_hooks->RequestSharedHook
        <Functions::_ZN10CNWSDialog9RunScriptEP10CNWSObjectRK7CResRef,
            void, CNWSDialog *, CNWSObject*, const CResRef*>(&Hooks::RunScript);
    GetServices()->m_hooks->RequestExclusiveHook<Functions::_ZN10CNWSDialog10GetSpeakerEP10CNWSObjectRK10CExoString>(&Hooks::GetSpeaker);

    m_GetSpeakerHook = GetServices()->m_hooks->FindHookByAddress(Functions::_ZN10CNWSDialog10GetSpeakerEP10CNWSObjectRK10CExoString);
    
}

Dialog::~Dialog()
{
}

ArgumentStack Dialog::GetCurrentNodeType(ArgumentStack&&)
{
    int32_t retval;
    switch (statestack[ssp])
    {
        case DIALOG_STATE_START:        retval = NODE_TYPE_STARTING_NODE; break;
        case DIALOG_STATE_SEND_ENTRY:   retval = NODE_TYPE_ENTRY_NODE;    break;
        case DIALOG_STATE_HANDLE_REPLY: retval = NODE_TYPE_REPLY_NODE;    break;
        case DIALOG_STATE_SEND_REPLIES: retval = NODE_TYPE_REPLY_NODE;    break;
        default: retval = NODE_TYPE_INVALID;                              break;
    }

    return Services::Events::Arguments(retval);
}

ArgumentStack Dialog::GetCurrentScriptType(ArgumentStack&&)
{
    return Services::Events::Arguments(scriptType);
}

ArgumentStack Dialog::GetCurrentNodeID(ArgumentStack&&)
{
    int32_t retval;

    switch (statestack[ssp])
    {
        case DIALOG_STATE_START:
            retval = pDialog->m_pStartingEntries[loopCount].m_nIndex;
            break;
        case DIALOG_STATE_SEND_ENTRY:
            retval = idxEntry;
            break;
        case DIALOG_STATE_HANDLE_REPLY:
            retval = pDialog->m_pEntries[idxEntry].m_pReplies[idxReply].m_nIndex;
            break;
        case DIALOG_STATE_SEND_REPLIES:
            retval = pDialog->m_pEntries[pDialog->m_currentEntryIndex].m_pReplies[loopCount].m_nIndex;
            break;
        default:
            retval = -1;
            break;
    }

    return Services::Events::Arguments(retval);
}

ArgumentStack Dialog::GetCurrentNodeIndex(ArgumentStack&&)
{
    return Services::Events::Arguments(loopCount);
}

ArgumentStack Dialog::GetCurrentNodeText(ArgumentStack&& args)
{
    CExoString str;

    auto language = Services::Events::ExtractArgument<int32_t>(args);
    auto gender = Services::Events::ExtractArgument<int32_t>(args);
    CExoLocString *pLocString;

    switch (statestack[ssp])
    {
        case DIALOG_STATE_START:
            pLocString = &pDialog->m_pEntries[pDialog->m_pStartingEntries[loopCount].m_nIndex].m_sText;
            break;
        case DIALOG_STATE_SEND_ENTRY:
            pLocString = &pDialog->m_pEntries[idxEntry].m_sText;
            break;
        case DIALOG_STATE_HANDLE_REPLY:
        {
            auto idx = pDialog->m_pEntries[idxEntry].m_pReplies[idxReply].m_nIndex;
            pLocString = &pDialog->m_pReplies[idx].m_sText;
            break;
        }
        case DIALOG_STATE_SEND_REPLIES:
        {
            auto idx = pDialog->m_pEntries[pDialog->m_currentEntryIndex].m_pReplies[loopCount].m_nIndex;
            pLocString = &pDialog->m_pReplies[idx].m_sText;
            break;
        }
        default:
            pLocString = nullptr;
            break;
    }

    if (pLocString)
    {
        gender = !!gender; // Only male/female supported; CExoLocString is traditional that way.
        pLocString->GetString(language, &str, gender, true);
    }

    return Services::Events::Arguments(std::string(str.CStr()));
}

ArgumentStack Dialog::SetCurrentNodeText(ArgumentStack&& args)
{
    auto str = Services::Events::ExtractArgument<std::string>(args);
    auto language = Services::Events::ExtractArgument<int32_t>(args);
    auto gender = Services::Events::ExtractArgument<int32_t>(args);
    CExoLocString *pLocString;

    switch (statestack[ssp])
    {
        case DIALOG_STATE_START:
            pLocString = &pDialog->m_pEntries[pDialog->m_pStartingEntries[loopCount].m_nIndex].m_sText;
            break;
        case DIALOG_STATE_SEND_ENTRY:
            pLocString = &pDialog->m_pEntries[idxEntry].m_sText;
            break;
        case DIALOG_STATE_HANDLE_REPLY:
        {
            auto idx = pDialog->m_pEntries[idxEntry].m_pReplies[idxReply].m_nIndex;
            pLocString = &pDialog->m_pReplies[idx].m_sText;
            break;
        }
        case DIALOG_STATE_SEND_REPLIES:
        {
            auto idx = pDialog->m_pEntries[pDialog->m_currentEntryIndex].m_pReplies[loopCount].m_nIndex;
            pLocString = &pDialog->m_pReplies[idx].m_sText;
            break;
        }
        default:
            pLocString = nullptr;
            break;
    }

    if (pLocString)
    {
        CExoString cexostr = str.c_str();
        gender = !!gender; // Only male/female supported; CExoLocString is traditional that way.
        pLocString->AddString(language, cexostr, gender);
    }

    return Services::Events::Arguments();
}

ArgumentStack Dialog::End(ArgumentStack&& args)
{
    auto oidObject = Services::Events::ExtractArgument<Types::ObjectID >(args);
      ASSERT_OR_THROW(oidObject != Constants::OBJECT_INVALID);

    if (auto *pObject = Utils::AsNWSObject(Utils::GetGameObject(oidObject)))
    {
        pObject->StopDialog();
    }

    return Services::Events::Arguments();
}

ArgumentStack Dialog::SetNPCSpeaker(ArgumentStack&& args)
{
    auto oidObject = Services::Events::ExtractArgument<Types::ObjectID>(args);
    ASSERT_OR_THROW(oidObject != Constants::OBJECT_INVALID);
    
    g_plugin->newSpeaker = oidObject;
    uint32_t numSpeakers = pDialog->m_nSpeakerMap;
    CNWSDialogSpeaker** pSpeakers = &(pDialog->m_pSpeakerMap);

    if (pSpeakers != nullptr) delete[] *pSpeakers;
    LOG_DEBUG("Deleted pSpeakers");
    *pSpeakers = new CNWSDialogSpeaker;   
    LOG_DEBUG("New DialogSpeaker created");
    *pSpeakers->m_id = oidObject;
    LOG_DEBUG("New id: %n", *pSpeakers->m_id);
    *pSpeakers->m_sSpeaker = Utils::AsNWSObject(Utils::GetGameObject(oidObject))->m_sTag;
    LOG_DEBUG("New tag: %s", *pSpeakers->m_sSpeaker);
    pDialog->m_nSpeakerMap = 1;
    LOG_DEBUG("Speaker map size is %n", pDialog->m_nSpeakerMap);       

    return Services::Events::Arguments();
}

}
