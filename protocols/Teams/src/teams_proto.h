#define TEAMS_CLIENT_ID  "8ec6bc83-69c8-4392-8f08-b3c986009232"
#define TEAMS_CLIENTINFO_NAME "skypeteams"
#define TEAMS_CLIENTINFO_VERSION "49/24062722442"

#define TEAMS_BASE_HOST "teams.live.com"

#define TEAMS_USER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0 Teams/24165.1410.2974.6689/49"

#define DBKEY_ID       "id"
#define DBKEY_GROUP    "DefaultGroup"
#define DBKEY_RTOKEN   "RefreshToken"

struct COwnMessage
{
	COwnMessage(int _1, int64_t _2) :
		hMessage(_1),
		hClientMessageId(_2)
	{}

	COwnMessage(const char *pszText) :
		hMessage(0),
		hClientMessageId(0),
		szMessage(mir_strdup(pszText))
	{}

	int hMessage;
	int64_t hClientMessageId, iTimestamp = -1;
	ptrA szMessage;
};

struct CSkypeTransfer
{
	CMStringA docId, fileName, fileType, url;
	int iFileSize = 0, iWidth = -1, iHeight = -1;
};

class CTeamsProto : public PROTO<CTeamsProto>
{
	friend class COptionsMainDlg;
	friend class CDeviceCodeDlg;

	friend class CSkypeOptionsMain;
	friend class CSkypeGCCreateDlg;
	friend class CSkypeInviteDlg;
	friend class CMoodDialog;
	friend class CDeviceCodeDlg;

	class CTeamsProtoImpl
	{
		friend class CTeamsProto;
		CTeamsProto &m_proto;

		CTimer m_heartBeat, m_loginPoll;
		void OnHeartBeat(CTimer *)
		{
			m_proto.TRouterSendJson("ping");
		}
		void OnLoginPoll(CTimer *)
		{
			m_proto.LoginPoll();
		}

		CTeamsProtoImpl(CTeamsProto &pro) :
			m_proto(pro),
			m_heartBeat(Miranda_GetSystemWindow(), UINT_PTR(this) + 1),
			m_loginPoll(Miranda_GetSystemWindow(), UINT_PTR(this) + 2)
		{
			m_heartBeat.OnEvent = Callback(this, &CTeamsProtoImpl::OnHeartBeat);
			m_loginPoll.OnEvent = Callback(this, &CTeamsProtoImpl::OnLoginPoll);
		}
	} m_impl;

public:
	// constructor
	CTeamsProto(const char *protoName, const wchar_t *userName);
	~CTeamsProto();

	//////////////////////////////////////////////////////////////////////////////////////
	// Virtual functions

	MCONTACT AddToList(int flags, PROTOSEARCHRESULT* psr) override;
	MCONTACT AddToListByEvent(int flags, int iContact, MEVENT hDbEvent) override;
	int      AuthRequest(MCONTACT hContact, const wchar_t* szMessage) override;
	int      Authorize(MEVENT hDbEvent) override;
	int      AuthDeny(MEVENT hDbEvent, const wchar_t* szReason) override;
	int      AuthRecv(MCONTACT hContact, DB::EventInfo &dbei) override;
	INT_PTR  GetCaps(int type, MCONTACT hContact = NULL) override;
	int      GetInfo(MCONTACT hContact, int infoType) override;
	HANDLE   SearchBasic(const wchar_t* id) override;
	int      SendMsg(MCONTACT hContact, MEVENT hReplyEvent, const char* msg) override;
	int      SetStatus(int iNewStatus) override;
	int      UserIsTyping(MCONTACT hContact, int type) override;
	int      RecvContacts(MCONTACT hContact, DB::EventInfo &dbei) override;
	HANDLE   SendFile(MCONTACT hContact, const wchar_t *szDescription, wchar_t **ppszFiles) override;

	void     OnBuildProtoMenu(void) override;
	bool     OnContactDeleted(MCONTACT, uint32_t flags) override;
	MWindow  OnCreateAccMgrUI(MWindow) override;
	void     OnEventEdited(MCONTACT hContact, MEVENT hDbEvent, const DBEVENTINFO &dbei) override;
	void     OnEventDeleted(MCONTACT hContact, MEVENT hDbEvent, int flags) override;
	void     OnMarkRead(MCONTACT, MEVENT) override;
	void     OnModulesLoaded() override;
	void     OnReceiveOfflineFile(DB::EventInfo &dbei, DB::FILE_BLOB &blob) override;
	void     OnShutdown() override;

	// menus
	static void InitMenus();

	// popups
	void InitPopups();
	void UninitPopups();

	// search

	//////////////////////////////////////////////////////////////////////////////////////
	// settings

	CMOption<bool> m_bAutoHistorySync;
	CMOption<bool> m_bUseBBCodes;

	CMOption<bool> m_bUseHostnameAsPlace;
	CMOption<wchar_t*> m_wstrPlace;

	CMOption<wchar_t*> m_wstrCListGroup;

	CMOption<uint8_t> m_iMood;
	CMOption<wchar_t*> m_wstrMoodMessage, m_wstrMoodEmoji;

	//////////////////////////////////////////////////////////////////////////////////////
	// other data

	int m_iPollingId, m_iMessageId = 1;
	CMStringA m_szSkypename, m_szMyname, m_szOwnSkypeId, m_szSkypeToken, m_szEndpoint, m_szApiCookie;
	MCONTACT m_hMyContact;

	__forceinline CMStringA getId(MCONTACT hContact) {
		return getMStringA(hContact, DBKEY_ID);
	}

private:
	bool m_bHistorySynced;

	static std::map<std::wstring, std::wstring> languages;

	LIST<void> m_PopupClasses;

	// avatars
	bool ReceiveAvatar(MCONTACT hContact);
	void OnReceiveAvatar(MHttpResponse *response, AsyncHttpRequest *pRequest);

	void SetAvatarUrl(MCONTACT hContact, const CMStringW &tszUrl);

	void ReloadAvatarInfo(MCONTACT hContact);
	void GetAvatarFileName(MCONTACT hContact, wchar_t *pszDest, size_t cbLen);

	INT_PTR __cdecl SvcGetAvatarInfo(WPARAM, LPARAM);
	INT_PTR __cdecl SvcGetAvatarCaps(WPARAM, LPARAM);
	INT_PTR __cdecl SvcGetMyAvatar(WPARAM, LPARAM);

	void OnSentAvatar(MHttpResponse *response, AsyncHttpRequest *pRequest);
	INT_PTR __cdecl SvcSetMyAvatar(WPARAM, LPARAM);

	// chats
	void InitGroupChatModule();

	int __cdecl OnGroupChatEventHook(WPARAM, LPARAM lParam);
	int __cdecl OnGroupChatMenuHook(WPARAM, LPARAM lParam);
	INT_PTR __cdecl OnJoinChatRoom(WPARAM hContact, LPARAM);
	INT_PTR __cdecl OnLeaveChatRoom(WPARAM hContact, LPARAM);

	void OnGetChatInfo(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void GetChatInfo(const wchar_t *chatId);
	
	void OnGetChatMembers(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void GetChatMembers(const LIST<char> &ids, SESSION_INFO *si);

	SESSION_INFO *StartChatRoom(const wchar_t *tid, const wchar_t *tname, const char *pszVersion = nullptr);

	bool OnChatEvent(const JSONNode &node);
	wchar_t *GetChatContactNick(SESSION_INFO *si, const wchar_t *id, const wchar_t *name = nullptr, bool *isQualified = nullptr);

	bool AddChatContact(SESSION_INFO *si, const wchar_t *id, const wchar_t *role, bool isChange = false);
	void RemoveChatContact(SESSION_INFO *si, const wchar_t *id, const wchar_t *initiator = L"");
	void SendChatMessage(SESSION_INFO *si, const wchar_t *tszMessage);

	void InviteUserToChat(const char *chatId, const char *skypename, const char *role);
	void KickChatUser(const char *chatId, const char *userId);
	void SetChatProperty(const char *chatId, const char *propname, const char *value);
	void SetChatStatus(MCONTACT hContact, int iStatus);

	bool ParseMessage(const JSONNode &node, DB::EventInfo &dbei);

	// contacts
	void LoadContactsAuth(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnGotContactsInfo(MHttpResponse *response, AsyncHttpRequest *pRequest);

	void GetShortInfo(const OBJLIST<char> &ids);
	void RefreshContactsInfo();
	void SetContactStatus(MCONTACT hContact, uint16_t status);

	MCONTACT FindContact(const char *skypeId);
	MCONTACT FindContact(const wchar_t *skypeId);

	MCONTACT AddContact(const char *skypename, const char *nick, bool isTemporary = false);

	MCONTACT GetContactFromAuthEvent(MEVENT hEvent);

	INT_PTR __cdecl BlockContact(WPARAM hContact, LPARAM);
	void OnBlockContact(MHttpResponse *response, AsyncHttpRequest *pRequest);

	INT_PTR __cdecl UnblockContact(WPARAM hContact, LPARAM);
	void OnUnblockContact(MHttpResponse *response, AsyncHttpRequest *pRequest);

	// files
	void SendFile(CFileUploadParam *fup);
	void OnASMObjectCreated(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnASMObjectUploaded(MHttpResponse *response, AsyncHttpRequest *pRequest);

	void __cdecl ReceiveFileThread(void *param);

	INT_PTR __cdecl SvcOfflineFile(WPARAM, LPARAM);

	// history
	void FetchMissingHistory(const JSONNode &node, MCONTACT);
	void GetServerHistory(MCONTACT hContact, int pageSize, int64_t timestamp, bool bOperative);
	void RefreshConversations();

	void OnGetServerHistory(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnSyncConversations(MHttpResponse *response, AsyncHttpRequest *pRequest);

	// http queue
	bool m_isTerminated = true;
	mir_cs m_requestQueueLock;
	LIST<AsyncHttpRequest> m_requests;
	MEventHandle m_hRequestQueueEvent;
	HANDLE m_hRequestQueueThread;
	CMStringA m_szAccessToken, m_szSubstrateToken;

	void __cdecl WorkerThread(void *);

	void StartQueue();
	void StopQueue();

	MHttpResponse *DoSend(AsyncHttpRequest *request);

	void Execute(AsyncHttpRequest *request);
	void PushRequest(AsyncHttpRequest *request);

	// login
	CMStringW m_wszUserCode;
	CMStringA m_szDeviceCode, m_szDeviceCookie, m_szVerificationUrl;
	time_t m_iLoginExpires;

	void Login();
	void LoggedIn();
	void LoginPoll();
	void LoginError();

	void OnEndpointCreated(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnReceiveApiCookie(MHttpResponse *response, AsyncHttpRequest *pRequest);

	void OauthRefreshServices();
	void RefreshToken(const char *pszScope, AsyncHttpRequest::MTHttpRequestHandler pFunc);

	void OnReceiveSkypeToken(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnReceiveDevicePoll(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnReceiveDeviceToken(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnRefreshAccessToken(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnRefreshSkypeToken(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnRefreshSubstrate(MHttpResponse *response, AsyncHttpRequest *pRequest);

	// menus
	static HGENMENU ContactMenuItems[CMI_MAX];
	int OnPrebuildContactMenu(WPARAM hContact, LPARAM);
	static int PrebuildContactMenu(WPARAM hContact, LPARAM lParam);

	// messages
	mir_cs m_lckOutMessagesList;
	LIST<COwnMessage> m_OutMessages;

	void OnMessageSent(MHttpResponse *response, AsyncHttpRequest *pRequest);
	int SendServerMsg(MCONTACT hContact, const char *szMessage, int64_t iMessageId = 0);

	int __cdecl OnPreCreateMessage(WPARAM, LPARAM lParam);

	void ProcessContactRecv(MCONTACT hContact, const char *szContent, DB::EventInfo &dbei);
	void ProcessFileRecv(MCONTACT hContact, const char *szContent, DB::EventInfo &dbei);

	// options
	int __cdecl OnOptionsInit(WPARAM wParam, LPARAM lParam);

	// profile
	void OnGetProfileInfo(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void GetProfileInfo(MCONTACT hContact);

	void UpdateProfileDisplayName(const JSONNode &root, MCONTACT hContact = NULL);
	void UpdateProfileGender(const JSONNode &root, MCONTACT hContact = NULL);
	void UpdateProfileBirthday(const JSONNode &root, MCONTACT hContact = NULL);
	void UpdateProfileCountry(const JSONNode &node, MCONTACT hContact = NULL);
	void UpdateProfileEmails(const JSONNode &root, MCONTACT hContact = NULL);
	void UpdateProfileAvatar(const JSONNode &root, MCONTACT hContact = NULL);

	// search
	void OnSearch(MHttpResponse *response, AsyncHttpRequest *pRequest);

	// server requests
	void OnStatusChanged(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void SetServerStatus(int iStatus);

	void CreateContactSubscription();

	// utils
	__forceinline bool IsOnline() const
	{	return (m_iStatus > ID_STATUS_OFFLINE);
	}

	bool IsMe(const wchar_t *str);
	bool IsMe(const char *str);

	int64_t getLastTime(MCONTACT);
	void setLastTime(MCONTACT, int64_t);

	CMStringW RemoveHtml(const CMStringW &src);

	void ShowNotification(const wchar_t *message, MCONTACT hContact = NULL);
	void ShowNotification(const wchar_t *caption, const wchar_t *message, MCONTACT hContact = NULL, int type = 0);
	static bool IsFileExists(std::wstring path);

	static LRESULT CALLBACK PopupDlgProcCall(HWND hPopup, UINT uMsg, WPARAM wParam, LPARAM lParam);

	void SetString(MCONTACT hContact, const char *pszSetting, const JSONNode &node);

	CMStringW ChangeTopicForm();

	// services
	INT_PTR __cdecl OnRequestAuth(WPARAM hContact, LPARAM);
	INT_PTR __cdecl OnGrantAuth(WPARAM hContact, LPARAM);
	INT_PTR __cdecl SvcLoadHistory(WPARAM hContact, LPARAM);
	INT_PTR __cdecl SvcEmptyHistory(WPARAM hContact, LPARAM);
	INT_PTR __cdecl SvcCreateChat(WPARAM, LPARAM);
	INT_PTR __cdecl ParseSkypeUriService(WPARAM, LPARAM lParam);

	// trouter
public:
	void TRouterProcess(const char *str);

private:
	HNETLIBUSER m_hTrouterNetlibUser;
	CMStringA m_szTrouterUrl, m_szTrouterSurl;
	WebSocket<CTeamsProto> *m_ws;
	MHttpHeaders m_connectParams;
	int iCommandId;

	void ProcessEvent(const JSONNode &node);
	void ProcessNewMessage(const JSONNode &node);
	void ProcessUserPresence(const JSONNode &node);
	void ProcessThreadUpdate(const JSONNode &node);
	void ProcessServerMessage(const std::string &szName, int packetId, const JSONNode &args);
	void ProcessConversationUpdate(const JSONNode &node);

	void __cdecl GatewayThread(void *);
	void GatewayThreadWorker();

	void TRouterSendJson(const char *szName, const JSONNode *node = nullptr, int iReplyTo = -1);
	void TRouterSendJson(const JSONNode &node, int iReplyTo = -1);

	void TRouterSendActive(bool bActive, int iReplyTo = -1);
	void TRouterRegister();
	void TRouterRegister(const char *pszAppId, const char *pszKey, const char *pszPath, const char *pszContext);

	void StartTrouter();
	void StopTrouter();

	void OnTrouterInfo(MHttpResponse *response, AsyncHttpRequest *pRequest);
	void OnTrouterSession(MHttpResponse *response, AsyncHttpRequest *pRequest);
};

typedef CProtoDlgBase<CTeamsProto> CTeamsDlgBase;

struct CMPlugin : public ACCPROTOPLUGIN<CTeamsProto>
{
	CMPlugin();

	int Load() override;
	int Unload() override;
};
