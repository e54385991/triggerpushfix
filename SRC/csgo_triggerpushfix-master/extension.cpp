#pragma comment(lib, "legacy_stdio_definitions.lib")
#include "extension.h"

#include <map>
#include <cstdio>

#include "triggers_shared.h"

TriggerPushFix g_Fixer;

SMEXT_LINK(&g_Fixer);

SH_DECL_MANUALHOOK1_void(Touch, 0, 0, 0, CBaseEntity *);
SH_DECL_MANUALHOOK1_void(EndTouch, 0, 0, 0, CBaseEntity *);
SH_DECL_MANUALHOOK2_void(PlayerRunCmdHook, 0, 0, 0, CUserCmd *, IMoveHelper *);

#define PLAYERRUNCMD_OFFSET (476)
#define CBASEENTITY_TOUCH_OFFSET (103)
#define CBASEENTITY_ENDTOUCH_OFFSET (104)
#define TRIGGER_PASSESTRIGGERFILTERS_OFFSET (209)



IServerTools *servertools = NULL;

IBinTools* bintools = NULL;
ISDKHooks* sdkhooks = NULL;

ICallWrapper* passesfiltercall = NULL;

struct ExecPlayer
{
	ExecPlayer(int id) : hookid(id) {
		mutex = threader->MakeMutex();
	}
	~ExecPlayer() {
		if (hookid) {
			SH_REMOVE_HOOK_ID(hookid);
			hookid = 0;
		}
		mutex->DestroyThis();
	}
	bool CanPush(CBaseEntity* ent) {
		if (count.find(ent) == count.end()) {
			count[ent] = 0;
		}
		return count[ent] < 1;
	}
	void IncCount(CBaseEntity* ent) {
		if (count.find(ent) == count.end()) {
			count[ent] = 0;
		}
		count[ent]++;
	}
	void ResetCounts() {
		count.clear();
	}
public:
	IMutex *mutex;
	std::map<CBaseEntity*, unsigned int> count;
	int hookid;
};

struct TriggerHooker
{
	TriggerHooker(int touchid) : touchhook(touchid) {}
	~TriggerHooker() {
		if (touchhook) {
			SH_REMOVE_HOOK_ID(touchhook);
			touchhook = 0;
		}
	}
public:
	int touchhook;
};

ExecPlayer *g_ExecList[SM_MAXPLAYERS + 1];

std::map<CBaseEntity*, TriggerHooker*> g_HookList;

static CBaseEntity* FindEntityByClassname(CBaseEntity *pEntity, const char *searchname);

bool TriggerPushFix::SDK_OnMetamodLoad(ISmmAPI *ismm, char *error, size_t maxlength, bool late)
{
	// Macro fix
	size_t maxlen = maxlength;

	GET_V_IFACE_ANY(GetServerFactory, servertools, IServerTools, VSERVERTOOLS_INTERFACE_VERSION);
	return true;
}

bool TriggerPushFix::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	SH_MANUALHOOK_RECONFIGURE(Touch, CBASEENTITY_TOUCH_OFFSET, 0, 0);
	SH_MANUALHOOK_RECONFIGURE(EndTouch, CBASEENTITY_ENDTOUCH_OFFSET, 0, 0);
	SH_MANUALHOOK_RECONFIGURE(PlayerRunCmdHook, PLAYERRUNCMD_OFFSET, 0, 0);

	sharesys->AddDependency(myself, "bintools.ext", true, true);
	sharesys->AddDependency(myself, "sdkhooks.ext", true, true);
	if (!sharesys->RequestInterface(SMINTERFACE_BINTOOLS_NAME, SMINTERFACE_BINTOOLS_VERSION, myself, reinterpret_cast<SMInterface**>(&bintools))) {
		snprintf(error, maxlength, "Cannot get IBintools Interface");
		return false;
	}
	if (!sharesys->RequestInterface(SMINTERFACE_SDKHOOKS_NAME, SMINTERFACE_SDKHOOKS_VERSION, myself, reinterpret_cast<SMInterface**>(&sdkhooks))) {
		snprintf(error, maxlength, "Cannot get SDKHooks Interface");
		return false;
	}

	PassInfo retBuf;
	retBuf.type = PassType_Basic;
	retBuf.flags = PASSFLAG_BYVAL;
	retBuf.size = sizeof(bool);
	
	PassInfo paramInfo[1];
	paramInfo[0].type = PassType_Basic;
	paramInfo[0].flags = PASSFLAG_BYVAL;
	paramInfo[0].size = sizeof(CBaseEntity*);

	passesfiltercall = bintools->CreateVCall(TRIGGER_PASSESTRIGGERFILTERS_OFFSET, 0, 0, &retBuf, paramInfo, 1);

	g_ExecList[0] = NULL;
	for (int client = 1; client < SM_MAXPLAYERS + 1; client++) {
		g_ExecList[client] = NULL;
	}
	for (int client = 1; client < SM_MAXPLAYERS + 1; client++) {
		edict_t *edict = gamehelpers->EdictOfIndex(client);
		if (!edict || edict->IsFree()) {
			continue;
		}
		IGamePlayer *player = playerhelpers->GetGamePlayer(edict);
		if (!player || !player->IsInGame()) {
			continue;
		}
		IServerUnknown *unk = edict->GetUnknown();
		if (!unk) {
			continue;
		}
		CBaseEntity *pEntity = unk->GetBaseEntity();
		g_ExecList[client] = new ExecPlayer(SH_ADD_MANUALHOOK(PlayerRunCmdHook, pEntity, SH_MEMBER(&g_Fixer, &TriggerPushFix::Hook_PlayerRunCmd), false));
	}
	CBaseEntity *pEntity = NULL;
	while ((pEntity = FindEntityByClassname(pEntity, "trigger_push")) != NULL) {
		g_HookList[pEntity] = new TriggerHooker(SH_ADD_MANUALHOOK(Touch, pEntity, SH_MEMBER(&g_Fixer, &TriggerPushFix::Hook_Touch), false));
	}

	sdkhooks->AddEntityListener(this);
	playerhelpers->AddClientListener(this);

	return true;
}

void TriggerPushFix::SDK_OnUnload()
{
	sdkhooks->RemoveEntityListener(this);
	playerhelpers->RemoveClientListener(this);

	for (int client = 1; client < SM_MAXPLAYERS + 1; client++) {
		if (g_ExecList[client]) {
			delete g_ExecList[client];
			g_ExecList[client] = NULL;
		}
	}
	for (auto it = g_HookList.begin(); it != g_HookList.end(); it++) {
		delete it->second;
	}
	g_HookList.clear();

	passesfiltercall->Destroy();
}

void TriggerPushFix::OnEntityCreated(CBaseEntity *pEntity, const char *classname)
{
	if (!strcmp(classname, "trigger_push")) {
		g_HookList[pEntity] = new TriggerHooker(SH_ADD_MANUALHOOK(Touch, pEntity, SH_MEMBER(&g_Fixer, &TriggerPushFix::Hook_Touch), false));
	}
}

void TriggerPushFix::OnEntityDestroyed(CBaseEntity *pEntity)
{
	if (g_HookList.find(pEntity) != g_HookList.end()) {
		delete g_HookList[pEntity];
		g_HookList.erase(pEntity);
	}
}

void TriggerPushFix::OnClientPutInServer(int client)
{
	edict_t *edict = gamehelpers->EdictOfIndex(client);
	if (!edict || edict->IsFree()) {
		return;
	}
	IGamePlayer *player = playerhelpers->GetGamePlayer(edict);
	if (!player) {
		return;
	}
	IServerUnknown *unk = edict->GetUnknown();
	if (!unk) {
		return;
	}
	CBaseEntity *pEntity = unk->GetBaseEntity();
	g_ExecList[client] = new ExecPlayer(SH_ADD_MANUALHOOK(PlayerRunCmdHook, pEntity, SH_MEMBER(&g_Fixer, &TriggerPushFix::Hook_PlayerRunCmd), false));
}

void TriggerPushFix::OnClientDisconnecting(int client)
{
	if (!g_ExecList[client]) {
		return;
	}
	delete g_ExecList[client];
	g_ExecList[client] = NULL;
}

void TriggerPushFix::Hook_Touch(CBaseEntity *pOther)
{
	int other = gamehelpers->EntityToBCompatRef(pOther);
	if (other < 1 || other > playerhelpers->GetMaxClients()) {
		RETURN_META(MRES_IGNORED);
	}

	CBaseEntity* pEntity = META_IFACEPTR(CBaseEntity);
	int entity = gamehelpers->EntityToBCompatRef(pEntity);

	CBaseEntity **params = reinterpret_cast<CBaseEntity**>(new char[sizeof(CBaseEntity*)*2]);
	params[0] = pEntity;
	params[1] = pOther;

	bool *passes_ptr = new bool;
	*passes_ptr = false;
	passesfiltercall->Execute(params, passes_ptr);

	bool passes = *passes_ptr;
	
	delete params;
	delete passes_ptr;

	if (!passes) {
		RETURN_META(MRES_IGNORED);
	}

	sm_datatable_info_t info;
	
	if (!gamehelpers->FindDataMapInfo(gamehelpers->GetDataMap(pOther), "m_spawnflags", &info)) {
		RETURN_META(MRES_IGNORED);
	}
	int spawnflags = *(int*)((char*)pEntity + info.actual_offset);
	if (spawnflags & SF_TRIG_PUSH_ONCE) {
		RETURN_META(MRES_IGNORED);
	}

	if (!gamehelpers->FindDataMapInfo(gamehelpers->GetDataMap(pOther), "m_MoveType", &info)) {
		RETURN_META(MRES_IGNORED);
	}
	MoveType_t movetype = (MoveType_t)*(char*)((char *)pOther + info.actual_offset);
	
	switch (movetype) {
	case MOVETYPE_NONE:
	case MOVETYPE_PUSH:
	case MOVETYPE_NOCLIP:
	case MOVETYPE_VPHYSICS:
	{
		RETURN_META(MRES_IGNORED);
	}
	}
	
	if (g_ExecList[other] == NULL) {
		RETURN_META(MRES_IGNORED);
	}
	g_ExecList[other]->mutex->Lock();
	if (g_ExecList[other]->CanPush(pEntity)) {
		g_ExecList[other]->IncCount(pEntity);
		g_ExecList[other]->mutex->Unlock();
		RETURN_META(MRES_IGNORED);
	}
	g_ExecList[other]->mutex->Unlock();
	RETURN_META(MRES_SUPERCEDE);
}

void TriggerPushFix::Hook_PlayerRunCmd(CUserCmd *ucmd, IMoveHelper *moveHelper)
{
	CBaseEntity* pEntity = META_IFACEPTR(CBaseEntity);
	int client = gamehelpers->EntityToBCompatRef(pEntity);
	if (client < 1 || client > playerhelpers->GetMaxClients()) {
		RETURN_META(MRES_IGNORED);
	}

	if (g_ExecList[client] == NULL) {
		return;
	}
	g_ExecList[client]->mutex->Lock();
	g_ExecList[client]->ResetCounts();
	g_ExecList[client]->mutex->Unlock();
	RETURN_META(MRES_IGNORED);
}

static CBaseEntity* FindEntityByClassname(CBaseEntity *pEntity, const char *searchname)
{
	if (!pEntity) {
		pEntity = (CBaseEntity *)servertools->FirstEntity();
	}
	else {
		pEntity = (CBaseEntity *)servertools->NextEntity(pEntity);
	}

	// it's tough to find a good ent these days
	if (!pEntity) {
		return NULL;
	}

	const char *classname;
	int lastletterpos;

	static int offset = -1;
	if (offset == -1) {
		sm_datatable_info_t info;
		if (!gamehelpers->FindDataMapInfo(gamehelpers->GetDataMap(pEntity), "m_iClassname", &info)) {
			return NULL;
		}
		offset = info.actual_offset;
	}

	string_t s;
	while (pEntity) {
		if ((s = *(string_t *)((uint8_t *)pEntity + offset)) == NULL_STRING) {
			pEntity = (CBaseEntity *)servertools->NextEntity(pEntity);
			continue;
		}

		classname = STRING(s);

		lastletterpos = strlen(searchname) - 1;
		if (searchname[lastletterpos] == '*') {
			if (strncasecmp(searchname, classname, lastletterpos) == 0) {
				return pEntity;
			}
		}
		else if (strcasecmp(searchname, classname) == 0) {
			return pEntity;
		}

		pEntity = (CBaseEntity *)servertools->NextEntity(pEntity);
	}
	return NULL;
}