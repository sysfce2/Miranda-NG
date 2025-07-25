/*

Jabber Protocol Plugin for Miranda NG

Copyright (c) 2002-04  Santithorn Bunchua
Copyright (c) 2005-12  George Hazan
Copyright (c) 2007     Maxim Mluhov
Copyright (C) 2012-25 Miranda NG team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"

#include <windns.h>   // requires Windows Platform SDK

#include "jabber_list.h"
#include "jabber_iq.h"
#include "jabber_caps.h"
#include "jabber_privacy.h"
#include "jabber_rc.h"
#include "jabber_proto.h"

#ifndef DNS_TYPE_SRV
#define DNS_TYPE_SRV 0x0021
#endif

static ThreadData *g_pRegInfo;  // pointer to registration thread

// <iq/> identification number for various actions
// for JABBER_REGISTER thread
int iqIdRegGetReg;
int iqIdRegSetReg;

struct JabberPasswordDlgParam
{
	CJabberProto *pro;

	BOOL     saveOnlinePassword;
	wchar_t  onlinePassword[128];
	char*    pszJid;
};

static INT_PTR CALLBACK JabberPasswordDlgProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	JabberPasswordDlgParam *param = (JabberPasswordDlgParam *)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);

	switch (msg) {
	case WM_INITDIALOG:
		TranslateDialogDefault(hwndDlg);

		param = (JabberPasswordDlgParam *)lParam;
		SetWindowLongPtr(hwndDlg, GWLP_USERDATA, lParam);

		wchar_t text[512];
		mir_snwprintf(text, TranslateT("Enter password for %s"), Utf2T(param->pszJid).get());
		SetDlgItemText(hwndDlg, IDC_JID, text);

		CheckDlgButton(hwndDlg, IDC_SAVEPASSWORD, param->pro->getByte("SaveSessionPassword", 0) ? BST_CHECKED : BST_UNCHECKED);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_SAVE_PERM:
			EnableWindow(GetDlgItem(hwndDlg, IDC_SAVEPASSWORD), BST_UNCHECKED == IsDlgButtonChecked(hwndDlg, IDC_SAVE_PERM));
			break;
		case IDOK:
			param->saveOnlinePassword = IsDlgButtonChecked(hwndDlg, IDC_SAVEPASSWORD);
			param->pro->setByte("SaveSessionPassword", param->saveOnlinePassword);

			GetDlgItemText(hwndDlg, IDC_PASSWORD, param->onlinePassword, _countof(param->onlinePassword));
			{
				BOOL savePassword = IsDlgButtonChecked(hwndDlg, IDC_SAVE_PERM);
				param->pro->setByte("SavePassword", savePassword);
				if (savePassword) {
					param->pro->setWString("Password", param->onlinePassword);
					param->saveOnlinePassword = true;
				}
			}
			__fallthrough;

		case IDCANCEL:
			EndDialog(hwndDlg, LOWORD(wParam));
			DestroyWindow(hwndDlg);
			return TRUE;
		}
		break;
	}

	return FALSE;
}

static INT_PTR CALLBACK JabberPasswordCreateDialogApcProc(void *param)
{
	return DialogBoxParam(g_plugin.getInst(), MAKEINTRESOURCE(IDD_PASSWORD), nullptr, JabberPasswordDlgProc, (LPARAM)param);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Jabber keep-alive thread

void CJabberProto::OnPingReply(const TiXmlElement *, CJabberIqInfo *pInfo)
{
	if (!pInfo)
		return;

	if (pInfo->GetIqType() == JABBER_IQ_TYPE_FAIL) {
		// disconnect because of timeout
		if (m_bEnableStreamMgmt)
			m_StrmMgmt.SendAck();
		m_ThreadInfo->send("</stream:stream>");
		m_ThreadInfo->shutdown();
	}
}

/////////////////////////////////////////////////////////////////////////////////////////

void CJabberProto::JLoginFailed(int errorCode)
{
	m_savedPassword = nullptr;
	ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, errorCode);
}

/////////////////////////////////////////////////////////////////////////////////////////

void CJabberProto::CheckKeepAlive()
{
	if (m_ThreadInfo) {
		if (m_bKeepAlive && ::GetTickCount() > m_ThreadInfo->lastWriteTime + m_iConnectionKeepAliveInterval) {
			if (m_ThreadInfo->jabberServerCaps & JABBER_CAPS_PING) {
				CJabberIqInfo *pInfo = AddIQ(&CJabberProto::OnPingReply, JABBER_IQ_TYPE_GET, nullptr, this);
				pInfo->SetTimeout(m_iConnectionKeepAliveTimeout);
				m_ThreadInfo->send(XmlNodeIq(pInfo) << XATTR("from", m_ThreadInfo->fullJID) << XCHILDNS("ping", JABBER_FEAT_PING));
			}
			else m_ThreadInfo->send(" \t ");
		}
	}

	// ugly hack to cut out group chat history: 2 seconds delay to skip all old messages
	// unfortunately there's no other way to detect if that message came online or from history
	time_t now = time(0);
	LISTFOREACH(i, this, LIST_CHATROOM)
		if (auto *item = ListGetItemPtrFromIndex(i))
			if (!item->bChatLogging && now - item->iChatInitTime > 2)
				item->bChatLogging = true;

	// check expired iq requests
	m_iqManager.CheckExpired();

	// check expired ad-hoc requests
	m_adhocManager.ExpireSessions();
}

/////////////////////////////////////////////////////////////////////////////////////////

static int CompareDNS(const DNS_SRV_DATAA *dns1, const DNS_SRV_DATAA *dns2)
{
	return (int)dns1->wPriority - (int)dns2->wPriority;
}

void ThreadData::xmpp_client_query(void)
{
	if (inet_addr(conn.server) != INADDR_NONE)
		return;

	char temp[256];
	mir_snprintf(temp, "_xmpp%s-client._tcp.%s", conn.useSSL ? "s" : "", conn.server);

	DNS_RECORDA *results = nullptr;
	DNS_STATUS status = DnsQuery_A(temp, DNS_TYPE_SRV, DNS_QUERY_STANDARD, nullptr, (PDNS_RECORD *)&results, nullptr);
	if (SUCCEEDED(status) && results) {
		LIST<DNS_SRV_DATAA> dnsList(5, CompareDNS);

		for (DNS_RECORDA *rec = results; rec; rec = rec->pNext)
			if (rec->Data.Srv.pNameTarget && rec->wType == DNS_TYPE_SRV)
				dnsList.insert(&rec->Data.Srv);

		for (auto &it : dnsList) {
			uint16_t dnsPort = (conn.port == 0 || conn.port == (conn.useSSL ? 5223 : 5222)) ? it->wPort : conn.port;
			char *dnsHost = it->pNameTarget;

			proto->debugLogA("%s resolved to %s:%d", temp, dnsHost, dnsPort);
			s = Netlib_OpenConnection(proto->m_hNetlibUser, dnsHost, dnsPort);
			if (s) {
				strncpy_s(conn.manualHost, dnsHost, _TRUNCATE);
				conn.port = dnsPort;
				break;
			}
		}
		DnsRecordListFree(results, DnsFreeRecordList);
	}
	else proto->debugLogA("%s not resolved", temp);
}

void CJabberProto::xmlStreamInitialize(char *szWhich)
{
	debugLogA("Stream will be initialized %s", szWhich);
	replaceStr(m_szXmlStreamToBeInitialized, szWhich);
}

void CJabberProto::xmlStreamInitializeNow(ThreadData *info)
{
	debugLogA("Stream is initializing %s",
		m_szXmlStreamToBeInitialized ? m_szXmlStreamToBeInitialized : "after connect");
	replaceStr(m_szXmlStreamToBeInitialized, 0);

	m_bStreamSent = true;
	XmlNode n("stream:stream");
	n << XATTR("xmlns", "jabber:client") << XATTR("to", info->conn.server) << XATTR("xmlns:stream", "http://etherx.jabber.org/streams");
	n.InsertFirstChild(n.NewDeclaration("xml version=\"1.0\" encoding=\"UTF-8\""));

	if (m_tszSelectedLang)
		n << XATTR("xml:lang", m_tszSelectedLang);

	if (!m_bDisable3920auth) {
		n << XATTR("version", "1.0");
		if (m_bEnableSasl2)
			n << XATTR("from", CMStringA(FORMAT, "%s@%s", info->conn.username, info->conn.server));
	}

	tinyxml2::XMLPrinter printer(0, true);
	n.Print(&printer);
	CMStringA buf(printer.CStr());
	if (buf.GetLength() > 2)
		buf.Delete(buf.GetLength() - 2, 1);

	info->send(buf.GetBuffer(), buf.GetLength());
}

void CJabberProto::ServerThread(JABBER_CONN_DATA *pParam)
{
	Thread_SetName("Jabber: ServerThread");

	// registration? execute thread once and forget
	if (pParam) {
		ThreadData info(this, pParam);
		ServerThreadStub(info);
		return;
	}

	// normal server connection, we will fetch all connection parameters
	// e.g. username, password, etc. from the database.
	if (m_ThreadInfo != nullptr) {
		debugLogA("Thread ended, another normal thread is running");
		return;
	}

	do {
		ThreadData info(this, pParam);
		if (!ServerThreadStub(info))
			break;
	} while (m_StrmMgmt.IsResumeIdPresent());

	// fully reset strm_mgmt state
	m_StrmMgmt.ResetState();

	// quit all chatrooms (will send a quit message)
	LISTFOREACH(i, this, LIST_CHATROOM)
		if (auto *item = ListGetItemPtrFromIndex(i)) {
			if (item->si)
				Chat_EmptyHistory(item->si);
			GcQuit(item, 0, nullptr);
		}

	ListRemoveList(LIST_CHATROOM);
	ListRemoveList(LIST_BOOKMARK);
	UI_SAFE_NOTIFY_HWND(m_hwndJabberAddBookmark, WM_PROTO_CHECK_ONLINE);
	WindowList_Broadcast(m_hWindowList, WM_PROTO_CHECK_ONLINE, 0, 0);

	// Set status to offline
	debugLogA("m_iDesiredStatus reset to (%d,%d) => %d", m_iStatus, m_iDesiredStatus, ID_STATUS_OFFLINE);
	int oldStatus = m_iStatus;
	m_iDesiredStatus = m_iStatus = ID_STATUS_OFFLINE;
	ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)oldStatus, m_iStatus);

	// Set all contacts to offline
	debugLogA("leaving worker thread");
	for (auto &hContact : AccContacts())
		SetContactOfflineStatus(hContact);

	mir_free(m_szJabberJID);
	m_szJabberJID = nullptr;
	m_tmJabberLoggedInTime = 0;
	ListWipe();

	WindowList_Broadcast(m_hWindowList, WM_JABBER_REFRESH_VCARD, 0, 0);
}

bool CJabberProto::ServerThreadStub(ThreadData &info)
{
	debugLogA("Thread started: type=%d", info.bIsReg);

	if (m_bManualConnect == TRUE) {
		ptrA szManualHost(getStringA("ManualHost"));
		if (szManualHost != nullptr)
			strncpy_s(info.conn.manualHost, szManualHost, _TRUNCATE);

		info.conn.port = getWord("ManualPort");
	}
	else info.conn.port = getWord("Port");

	info.conn.useSSL = m_bUseSSL;
	if (!info.conn.port)
		info.conn.port = info.conn.useSSL ? 5223 : 5222;

	if (!info.bIsReg) {
		m_ThreadInfo = &info;

		if (char *pszValue = getUStringA("LoginName")) {
			strncpy_s(info.conn.username, pszValue, _TRUNCATE);
			mir_free(pszValue);
		}

		if (*rtrim(info.conn.username) == '\0') {
			DWORD dwSize = _countof(info.conn.username);
			if (GetUserNameA(info.conn.username, &dwSize))
				setString("LoginName", info.conn.username);
			else
				info.conn.username[0] = 0;
		}

		if (*rtrim(info.conn.username) == '\0') {
			debugLogA("Thread ended, login name is not configured");
			JLoginFailed(LOGINERR_BADUSERID);

			return false;
		}

		ptrA szValue(getStringA("LoginServer"));
		if (szValue != nullptr)
			strncpy_s(info.conn.server, szValue, _TRUNCATE);
		else {
			ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, LOGINERR_NONETWORK);
			debugLogA("Thread ended, login server is not configured");
			return false;
		}

		if (m_bHostNameAsResource) {
			DWORD dwCompNameLen = _countof(info.resource) - 1;
			if (!GetComputerNameA(info.resource, &dwCompNameLen))
				strncpy_s(info.resource, "Miranda", _TRUNCATE);
		}
		else {
			ptrA tszValue(getUStringA("Resource"));
			if (tszValue != nullptr)
				strncpy_s(info.resource, tszValue, _TRUNCATE);
			else
				mir_strcpy(info.resource, "Miranda");
		}

		char jidStr[512];
		mir_snprintf(jidStr, "%s@%s/%s", info.conn.username, info.conn.server, info.resource);
		strncpy_s(info.fullJID, jidStr, _TRUNCATE);

		if (m_bUseDomainLogin) // in the case of NTLM auth we have no need in password
			info.conn.password[0] = 0;
		else if (!m_bSavePassword) { // we have to enter a password manually. have we done it before?
			if (m_savedPassword != nullptr)
				strncpy_s(info.conn.password, T2Utf(m_savedPassword), _TRUNCATE);
			else {
				mir_snprintf(jidStr, "%s@%s", info.conn.username, info.conn.server);

				JabberPasswordDlgParam param;
				param.pro = this;
				param.pszJid = jidStr;

				if (CallFunctionSync(JabberPasswordCreateDialogApcProc, &param) == IDCANCEL) {
					JLoginFailed(LOGINERR_BADUSERID);
					debugLogA("Thread ended, password request dialog was canceled");
					return false;
				}

				m_savedPassword = (param.saveOnlinePassword) ? mir_wstrdup(param.onlinePassword) : nullptr;
				strncpy_s(info.conn.password, T2Utf(param.onlinePassword), _TRUNCATE);
			}
		}
		else {
			ptrA tszPassw(getUStringA("Password"));
			if (tszPassw == nullptr) {
				JLoginFailed(LOGINERR_BADUSERID);
				debugLogA("Thread ended, password is not configured");
				return false;
			}
			strncpy_s(info.conn.password, tszPassw, _TRUNCATE);
		}
	}
	else {
		// Register new user connection, all connection parameters are already filled in.
		// Multiple threads allowed, although not possible :)
		// Thinking again. The multiple threads should not be allowed.
		info.reg_done = false;
		info.conn.SetProgress(25, TranslateT("Connecting..."));
		iqIdRegGetReg = -1;
		iqIdRegSetReg = -1;
	}

	int jabberNetworkBufferSize = 2048;
	if ((info.buffer = (char *)mir_alloc(jabberNetworkBufferSize + 1)) == nullptr) {	// +1 is for '\0' when debug logging this buffer
		debugLogA("Cannot allocate network buffer, thread ended");
		if (info.bIsReg)
			info.conn.SetProgress(100, TranslateT("Error: Not enough memory"));
		else
			ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, LOGINERR_NONETWORK);

		debugLogA("Thread ended, network buffer cannot be allocated");
		return false;
	}

	if (info.conn.manualHost[0] == 0) {
		NETLIBUSERSETTINGS us = {};
		Netlib_GetUserSettings(m_hNetlibUser, &us);
		if (us.useProxy && us.dnsThroughProxy)
			; // TODO implement dns request thru proxy
		else
			info.xmpp_client_query();

		if (info.s == nullptr) {
			strncpy_s(info.conn.manualHost, info.conn.server, _TRUNCATE);
			info.s = Netlib_OpenConnection(m_hNetlibUser, info.conn.manualHost, info.conn.port);
			if (info.s == nullptr && us.useProxy && us.dnsThroughProxy)
				PUShowMessageW(TranslateT("Requesting XMPP service address through proxy is not supported.\n"
					"Please allow direct DNS in options or specify the hostname manually"), SM_WARNING);
		}
	}
	else info.s = Netlib_OpenConnection(m_hNetlibUser, info.conn.manualHost, info.conn.port);

	debugLogA("Thread type=%d server='%s' port='%d'", info.bIsReg, info.conn.manualHost, info.conn.port);
	if (info.s == nullptr) {
		debugLogA("Connection failed (%d)", WSAGetLastError());
		if (!info.bIsReg) {
			if (m_ThreadInfo == &info)
				ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, LOGINERR_NONETWORK);
		}
		else info.conn.SetProgress(100, TranslateT("Error: Cannot connect to the server"));

		debugLogA("Thread ended, connection failed");
		return true;
	}

	// Determine local IP
	if (info.conn.useSSL) {
		debugLogA("Intializing SSL connection");
		if (!Netlib_StartSsl(info.s, info.conn.server)) {
			if (!info.bIsReg)
				ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, LOGINERR_NONETWORK);
			else
				info.conn.SetProgress(100, TranslateT("Error: Cannot connect to the server"));

			info.close();
			debugLogA("Thread ended, SSL connection failed");
			return false;
		}
	}

	if (!info.bIsReg) {
		// User may change status to OFFLINE while we are connecting above
		if (m_iDesiredStatus == ID_STATUS_OFFLINE)
			return false;

		m_szJabberJID = CMStringA(FORMAT, "%s@%s", info.conn.username, info.conn.server).Detach();
		setUString("jid", m_szJabberJID); // store jid in database

		ListInit();
		m_impl.m_keepAlive.Start(1000);
	}

	xmlStreamInitializeNow(&info);

	debugLogA("Entering main recv loop");
	int datalen = 0;

	// main socket reading cycle
	for (;;) {
		int recvResult = info.recv(info.buffer + datalen, jabberNetworkBufferSize - datalen);
		debugLogA("recvResult = %d", recvResult);
		if (recvResult <= 0)
			break;
		datalen += recvResult;

recvRest:
		info.buffer[datalen] = '\0';

		int bytesParsed = 0;
		TiXmlDocument root;
		if (m_bStreamSent) {
			m_bStreamSent = false;
			char *p = strstr(info.buffer, "<stream:stream");
			if (p) {
				char *q = strchr(p + 15, '>');
				if (q) {
					CMStringA tmp(info.buffer, int(q - info.buffer) + 1);
					tmp.Append("</stream:stream>");

					if (0 == root.Parse(tmp)) {
						for (auto *n : TiXmlEnum(&root))
							OnProcessProtocol(n, &info);
						bytesParsed = root.BytesParsed() - 16;
						debugLogA("bytesParsed = %d", bytesParsed);
					}
				}
			}
		}

		if (bytesParsed == 0) {
			if (0 == root.Parse(info.buffer)) {
				for (auto *n : TiXmlEnum(&root))
					OnProcessProtocol(n, &info);
				bytesParsed = root.BytesParsed();
				debugLogA("bytesParsed = %d", bytesParsed);
			}
			else debugLogA("parsing error %d: %s", root.ErrorID(), root.ErrorStr());
		}

		if (bytesParsed > 0) {
			if (bytesParsed < datalen)
				memmove(info.buffer, info.buffer + bytesParsed, datalen - bytesParsed);
			datalen -= bytesParsed;
		}
		else if (datalen >= jabberNetworkBufferSize) {
			jabberNetworkBufferSize *= 2;
			debugLogA("Increasing network buffer size to %d", jabberNetworkBufferSize);
			if ((info.buffer = (char *)mir_realloc(info.buffer, jabberNetworkBufferSize + 1)) == nullptr) {
				debugLogA("Cannot reallocate more network buffer, go offline now");
				break;
			}
		}
		else debugLogA("Unknown state: bytesParsed=%d, datalen=%d, jabberNetworkBufferSize=%d", bytesParsed, datalen, jabberNetworkBufferSize);

		if (m_szXmlStreamToBeInitialized)
			xmlStreamInitializeNow(&info);

		if (root.FirstChild() && datalen)
			goto recvRest;
	}

	if (!info.bIsReg) {
		m_impl.m_keepAlive.Stop();
		m_bJabberOnline = false;
		info.zlibUninit();
		EnableMenuItems(false);
		m_iqManager.ExpireAll();
		if (m_hwndJabberChangePassword)
			// Since this is a different thread, simulate the click on the cancel button instead
			SendMessage(m_hwndJabberChangePassword, WM_COMMAND, MAKEWORD(IDCANCEL, 0), 0);

		if (m_StrmMgmt.IsResumeIdPresent()) {
			m_StrmMgmt.HandleConnectionLost();
			int oldStatus = m_iStatus;
			m_iStatus = ID_STATUS_CONNECTING;
			ProtoBroadcastAck(0, ACKTYPE_STATUS, ACKRESULT_SUCCESS, (HANDLE)oldStatus, m_iStatus);
			info.close();
			return true;
		}
	}
	else {
		if (!info.reg_done)
			info.conn.SetProgress(100, TranslateT("Error: Connection lost"));
		g_pRegInfo = nullptr;
	}

	debugLogA("Thread ended: type=%d server='%s'", info.bIsReg, info.conn.server);

	info.close();
	debugLogA("Exiting ServerThread");
	return false;
}

void CJabberProto::PerformRegistration(ThreadData *info)
{
	iqIdRegGetReg = SerialNext();
	info->send(XmlNodeIq("get", iqIdRegGetReg, nullptr) << XQUERY(JABBER_FEAT_REGISTER));

	info->conn.SetProgress(50, TranslateT("Requesting registration instruction..."));
}

void CJabberProto::PerformIqAuth(ThreadData *info)
{
	if (!info->bIsReg) {
		info->send(XmlNodeIq(AddIQ(&CJabberProto::OnIqResultGetAuth, JABBER_IQ_TYPE_GET))
			<< XQUERY("jabber:iq:auth") << XCHILD("username", info->conn.username));
	}
	else PerformRegistration(info);
}

/////////////////////////////////////////////////////////////////////////////////////////

void CJabberProto::OnProcessStreamOpening(const TiXmlElement *node, ThreadData *info)
{
	if (mir_strcmp(node->Name(), "stream:stream"))
		return;

	if (!info->bIsReg) {
		const char *sid = XmlGetAttr(node, "id");
		if (sid != nullptr)
			info->szStreamId = mir_strdup(sid);
	}

	if (info->proto->m_bDisable3920auth)
		info->proto->PerformIqAuth(info);
}

void CJabberProto::PerformAuthentication(ThreadData *info)
{
	// no known mechanisms 
	if (m_arAuthMechs.getCount() == 0) {
		// if iq_auth is available, use it
		if (m_hasAuth) {
			m_hasAuth = false;
			PerformIqAuth(info);
			return;
		}

		char text[1024];
		mir_snprintf(text, TranslateU("Authentication failed for %s@%s."), info->conn.username, info->conn.server);
		MsgPopup(0, Utf2T(text), TranslateT("Authentication"));

		JLoginFailed(LOGINERR_WRONGPASSWORD);
		info->send("</stream:stream>");
		m_ThreadInfo = nullptr;
		return;
	}

	// grab the safest mechanism and proceed
	auto &auth = m_arAuthMechs[0];

	if (m_hasSasl2) {
		XmlNode node("authenticate");
		for (auto &it : m_arSaslUpgrade)
			node << XCHILD("upgrade", it->getName()) << XATTR("xmlns", "urn:xmpp:sasl:upgrade:0");
		node << XATTR("xmlns", JABBER_FEAT_SASL2) << XATTR("mechanism", auth.getName()) << XCHILD("initial-response", auth.getInitialRequest());
		info->send(node);
	}
	else info->send(XmlNode("auth", ptrA(auth.getInitialRequest())) << XATTR("xmlns", JABBER_FEAT_SASL) << XATTR("mechanism", auth.getName()));
}

/////////////////////////////////////////////////////////////////////////////////////////

void CJabberProto::OnProcessChannelBinding(const TiXmlElement *node)
{
	for (auto *it : TiXmlFilter(node, "channel-binding")) {
		if (auto *pszType = it->Attribute("type")) {
			if (!mir_strcmp(pszType, "tls-exporter"))
				m_bTlsExporter = true;
			else if (!mir_strcmp(pszType, "tls-server-end-point"))
				m_bTlsServerEndpoint = true;
		}
	}
}

void CJabberProto::OnProcessFeatures(const TiXmlElement *node, ThreadData *info)
{
	info->jabberServerCaps = JABBER_RESOURCE_CAPS_NONE;

	bool isRegisterAvailable = false;
	bool areMechanismsDefined = false;
	m_bTlsExporter = m_bTlsServerEndpoint = false;

	for (auto *n : TiXmlEnum(node)) {
		auto *pszName = n->Name();
		if (!mir_strcmp(pszName, "starttls")) {
			if (!info->conn.useSSL && m_bUseTLS) {
				debugLogA("Requesting TLS");
				info->send(XmlNode(pszName) << XATTR("xmlns", "urn:ietf:params:xml:ns:xmpp-tls"));
				return;
			}
		}

		if (!mir_strcmp(pszName, "compression") && m_bEnableZlib == TRUE) {
			debugLogA("Server compression available");
			for (auto *c : TiXmlFilter(n, "method")) {
				if (!mir_strcmp(c->GetText(), "zlib") && info->zlibInit()) {
					debugLogA("Requesting Zlib compression");
					info->send(XmlNode("compress") << XATTR("xmlns", "http://jabber.org/protocol/compress")
						<< XCHILD("method", "zlib"));
					return;
				}
			}
		}

		auto *xmlns = n->Attribute("xmlns");
		if (!mir_strcmp(pszName, "mechanisms")) {
			areMechanismsDefined = true;
			m_arAuthMechs.destroy();
			replaceStr(info->gssapiHostName, nullptr);

			for (auto *c : TiXmlEnum(n))
				OnProcessMechanism(c, info);
		}
		else if (!mir_strcmp(pszName, "register"))
			isRegisterAvailable = true;
		else if (!mir_strcmp(pszName, "auth"))
			m_hasAuth = true;
		else if (!mir_strcmp(pszName, "authentication") && !mir_strcmp(xmlns, JABBER_FEAT_SASL2) && m_bEnableSasl2) {
			m_hasSasl2 = areMechanismsDefined = true;
			m_arAuthMechs.destroy();
			m_arSaslUpgrade.destroy();
			replaceStr(info->gssapiHostName, nullptr);

			for (auto *c : TiXmlEnum(n)) {
				if (OnProcessMechanism(c, info))
					continue;

				if (!mir_strcmp(c->Name(), "inline")) {
					if (auto *sm = XmlGetChildByTag(c, "sm", "xmlns", JABBER_FEAT_SM))
						m_StrmMgmt.CheckStreamFeatures(sm);

					if (auto *bind = XmlGetChildByTag(c, "bind", "xmlns", "urn:xmpp:bind:0")) {
						// dunno why we need to handle that
					}
				}
				else if (!mir_strcmp(c->Name(), "upgrade") && c->Attribute("xmlns", "urn:xmpp:sasl:upgrade:0"))
					OnProcessUpgrade(c, info);
				else if (!mir_strcmp(pszName, "sasl-channel-binding") && c->Attribute("xmlns", JABBER_FEAT_CHANNEL_BINDING))
					OnProcessChannelBinding(c);
			}
		}
		else if (!mir_strcmp(pszName, "session"))
			m_hasSession = true;
		else if (m_bEnableStreamMgmt && !mir_strcmp(pszName, "sm"))
			m_StrmMgmt.CheckStreamFeatures(n);
		else if (!mir_strcmp(pszName, "csi") && !mir_strcmp(xmlns, JABBER_FEAT_CSI))
			m_bCisAvailable = true;
		else if (!mir_strcmp(pszName, "c") && !mir_strcmp(xmlns, JABBER_FEAT_ENTITY_CAPS)) {
			auto *szNode = n->Attribute("node"), *szHash = n->Attribute("ver");
			auto *pCaps = g_clientCapsManager.GetPartialCaps(szNode, szHash);
			if (pCaps == nullptr) {
				CMStringA payload(FORMAT, "%s%c%s", szNode, 0, szHash);
				info->pPendingQuery = payload.Detach();
			}
			else info->jabberServerCaps |= pCaps->GetCaps();
		}
		else if (!mir_strcmp(pszName, "sasl-channel-binding") && !mir_strcmp(xmlns, JABBER_FEAT_CHANNEL_BINDING))
			OnProcessChannelBinding(n);
	}

	if (areMechanismsDefined) {
		if (info->bIsReg)
			PerformRegistration(info);
		else
			PerformAuthentication(info);
		return;
	}

	// mechanisms are not defined.
	if (m_bEnableStreamMgmt && m_StrmMgmt.IsResumeIdPresent()) // resume should be done here
		m_StrmMgmt.CheckState();
	else {
		if (m_arAuthMechs.getCount()) { // we are already logged in
			info->send(
				XmlNodeIq(AddIQ(&CJabberProto::OnIqResultBind, JABBER_IQ_TYPE_SET))
				<< XCHILDNS("bind", JABBER_FEAT_BIND)
				<< XCHILD("resource", info->resource));
		}
		else // mechanisms are not available and we are not logged in
			PerformIqAuth(info);
	}
}

void CJabberProto::OnProcessFailure(const TiXmlElement *node, ThreadData *info)
{
	// failure xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\"
	const char *type = XmlGetAttr(node, "xmlns");
	if (!mir_strcmp(type, JABBER_FEAT_SASL) || !mir_strcmp(type, JABBER_FEAT_SASL2)) {
		m_arAuthMechs.remove(0L);
		PerformAuthentication(info);
	}
}

void CJabberProto::OnProcessFailed(const TiXmlElement *node, ThreadData *info) //used failed instead of failure, notes: https://xmpp.org/extensions/xep-0198.html#errors
{
	m_StrmMgmt.OnProcessFailed(node, info);
}

void CJabberProto::OnProcessEnabled(const TiXmlElement *node, ThreadData *info)
{
	if (m_bEnableStreamMgmt && !mir_strcmp(XmlGetAttr(node, "xmlns"), "urn:xmpp:sm:3"))
		m_StrmMgmt.OnProcessEnabled(node, info);
}

void CJabberProto::OnProcessError(const TiXmlElement *node, ThreadData *info)
{
	//failure xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\"
	if (!XmlFirstChild(node, 0))
		return;

	bool skipMsg = false;
	CMStringA buff;
	for (auto *n : TiXmlEnum(node)) {
		const char *name = n->Name();
		const char *desc = n->GetText();
		if (desc)
			buff.AppendFormat("%s: %s\r\n", name, desc);
		else
			buff.AppendFormat("%s\r\n", name);

		if (!mir_strcmp(name, "conflict"))
			ProtoBroadcastAck(0, ACKTYPE_LOGIN, ACKRESULT_FAILED, nullptr, LOGINERR_OTHERLOCATION);
		else if (!mir_strcmp(name, "see-other-host")) {
			skipMsg = true;
		}
	}
	if (!skipMsg)
		MsgPopup(0, Utf2T(buff), TranslateT("Error"));

	if (m_bEnableStreamMgmt) //TODO: check if needed/work here
		m_StrmMgmt.SendAck();
	info->send("</stream:stream>");
}

void CJabberProto::OnProcessSuccess(const TiXmlElement *node, ThreadData *info)
{
	const char *type;
	//	int iqId;
	// RECVED: <success ...
	// ACTION: if successfully logged in, continue by requesting a roster list and set my initial status
	if ((type = XmlGetAttr(node, "xmlns")) == nullptr)
		return;

	const char *pszFinal;
	if (!mir_strcmp(type, JABBER_FEAT_SASL))
		pszFinal = node->GetText();
	else if (!mir_strcmp(type, JABBER_FEAT_SASL2))
		pszFinal = XmlGetChildText(node, "additional-data");
	else {
		debugLogA("Success: unknown action %s.", type);
		return;
	}

	if (m_hasSasl2 && !pszFinal && info->m_saslUpgrade)
		pszFinal = info->m_saslUpgrade->getInitData();

	if (!m_arAuthMechs[0].validateLogin(pszFinal)) {
		info->send("</stream:stream>");
		return;
	}

	debugLogA("Success: Logged-in.");
	ptrA szNick(getUStringA("Nick"));
	if (!mir_strlen(szNick))
		setUString("Nick", info->conn.username);

	if (!m_hasSasl2)
		xmlStreamInitialize("after successful sasl");
}

void CJabberProto::OnProcessChallenge(const TiXmlElement *node, ThreadData *info)
{
	if (m_arAuthMechs.getCount() == 0) {
		debugLogA("No previous auth have been made, exiting...");
		return;
	}

	auto *xmlns = XmlGetAttr(node, "xmlns");
	if (mir_strcmp(xmlns, JABBER_FEAT_SASL) && mir_strcmp(xmlns, JABBER_FEAT_SASL2))
		return;

	char *challenge = m_arAuthMechs[0].getChallenge(node->GetText());
	info->send(XmlNode("response", challenge) << XATTR("xmlns", xmlns));
	mir_free(challenge);
}

void CJabberProto::OnProcessProtocol(const TiXmlElement *node, ThreadData *info)
{
	OnConsoleProcessXml(node, JCPF_IN);

	if (m_bEnableStreamMgmt)
		if (m_StrmMgmt.HandleIncommingNode(node))
			return;

	auto *pszName = node->Name();
	if (!mir_strcmp(pszName, "proceed"))
		OnProcessProceed(node, info);
	else if (!mir_strcmp(pszName, "compressed"))
		OnProcessCompressed(node, info);
	else if (!mir_strcmp(pszName, "stream:features"))
		OnProcessFeatures(node, info);
	else if (!mir_strcmp(pszName, "stream:stream"))
		OnProcessStreamOpening(node, info);
	else if (!mir_strcmp(pszName, "success"))
		OnProcessSuccess(node, info);
	else if (!mir_strcmp(pszName, "failure"))
		OnProcessFailure(node, info);
	else if (!mir_strcmp(pszName, "continue"))
		OnProcessContinue(node, info);
	else if (!mir_strcmp(pszName, "task-data"))
		OnProcessTaskData(node, info);
	else if (!mir_strcmp(pszName, "stream:error"))
		OnProcessError(node, info);
	else if (!mir_strcmp(pszName, "challenge"))
		OnProcessChallenge(node, info);
	else if (!info->bIsReg) {
		if (!mir_strcmp(pszName, "message")) {
			XmppMsg msg(node, info);
			MessageProcess(msg);
		}
		else if (!mir_strcmp(pszName, "presence"))
			OnProcessPresence(node, info);
		else if (!mir_strcmp(pszName, "iq"))
			OnProcessIq(node);
		else if (!mir_strcmp(pszName, "failed"))
			OnProcessFailed(node, info);
		else if (!mir_strcmp(pszName, "enabled"))
			OnProcessEnabled(node, info);
		else if (m_bEnableStreamMgmt && !mir_strcmp(pszName, "resumed"))
			m_StrmMgmt.OnProcessResumed(node, info);
		else
			debugLogA("Invalid top-level tag (only <message/> <presence/> and <iq/> allowed)");
	}
	else {
		if (!mir_strcmp(pszName, "iq"))
			OnProcessRegIq(node, info);
		else
			debugLogA("Invalid top-level tag (only <iq/> allowed)");
	}
}

void CJabberProto::OnProcessProceed(const TiXmlElement *node, ThreadData *info)
{
	const char *type;
	if ((type = XmlGetAttr(node, "xmlns")) != nullptr && !mir_strcmp(type, "error"))
		return;

	if (!mir_strcmp(type, "urn:ietf:params:xml:ns:xmpp-tls")) {
		debugLogA("Starting TLS...");

		if (!Netlib_StartSsl(info->s, info->conn.server)) {
			debugLogA("SSL initialization failed");
			info->send("</stream:stream>");
			info->shutdown();
		}
		else
			xmlStreamInitialize("after successful StartTLS");
	}
}

void CJabberProto::OnProcessCompressed(const TiXmlElement *node, ThreadData *info)
{
	debugLogA("Compression confirmed");

	const char *type = XmlGetAttr(node, "xmlns");
	if (type != nullptr && !mir_strcmp(type, "error"))
		return;
	if (mir_strcmp(type, "http://jabber.org/protocol/compress"))
		return;

	debugLogA("Starting Zlib stream compression...");

	info->useZlib = true;
	info->zRecvData = (char *)mir_alloc(ZLIB_CHUNK_SIZE);

	xmlStreamInitialize("after successful Zlib init");
}

void CJabberProto::OnProcessPubsubEvent(const TiXmlElement *node)
{
	const char *from = XmlGetAttr(node, "from");
	if (!from)
		return;

	auto *eventNode = XmlGetChildByTag(node, "event", "xmlns", JABBER_FEAT_PUBSUB_EVENT);
	if (!eventNode)
		return;

	m_pepServices.ProcessEvent(from, eventNode);

	MCONTACT hContact = HContactFromJID(from);
	if (!hContact)
		return;

	if (OmemoIsEnabled(hContact)) {
		auto *itemsNode = XmlGetChildByTag(eventNode, "items", "node", JABBER_FEAT_OMEMO ".devicelist");
		if (itemsNode) {
			OmemoHandleDeviceList(from, itemsNode);
			return;
		}
	}

	if (m_bEnableUserTune)
		if (auto *itemsNode = XmlGetChildByTag(eventNode, "items", "node", JABBER_FEAT_USER_TUNE)) {
			// node retract?
			if (XmlFirstChild(itemsNode, "retract")) {
				SetContactTune(hContact, nullptr, nullptr, nullptr, nullptr, nullptr);
				return;
			}

			auto *tuneNode = XmlGetChildByTag(XmlFirstChild(itemsNode, "item"), "tune", "xmlns", JABBER_FEAT_USER_TUNE);
			if (!tuneNode)
				return;

			const char *szArtist = XmlGetChildText(tuneNode, "artist");
			const char *szSource = XmlGetChildText(tuneNode, "source");
			const char *szTitle = XmlGetChildText(tuneNode, "title");
			const char *szTrack = XmlGetChildText(tuneNode, "track");

			wchar_t szLengthInTime[20];
			int nLength = XmlGetChildInt(tuneNode, "length");
			if (nLength > 0)
				mir_snwprintf(szLengthInTime, L"%02d:%02d:%02d", nLength / 3600, (nLength / 60) % 60, nLength % 60);

			SetContactTune(hContact, Utf2T(szArtist), nLength ? szLengthInTime : nullptr, Utf2T(szSource), Utf2T(szTitle), Utf2T(szTrack));
		}
}

MCONTACT CJabberProto::CreateTemporaryContact(const char *szJid, JABBER_LIST_ITEM *chatItem)
{
	if (chatItem == nullptr)
		return DBCreateContact(szJid, nullptr, true, true);

	const char *p = strchr(szJid, '/');
	if (p != nullptr && p[1] != '\0')
		p++;
	else
		p = szJid;
	MCONTACT hContact = DBCreateContact(szJid, p, true, false);

	pResourceStatus r(chatItem->findResource(p));
	if (r)
		setWord(hContact, "Status", r->m_iStatus);

	return hContact;
}

// XEP-0115: Entity Capabilities
void CJabberProto::OnProcessPresenceCapabilites(const TiXmlElement *node, pResourceStatus &r)
{
	if (r == nullptr)
		return;

	// already filled up? ok
	if (r->m_pCaps != nullptr)
		return;

	const char *from = XmlGetAttr(node, "from");
	if (from == nullptr)
		return;

	auto *n = XmlGetChildByTag(node, "c", "xmlns", JABBER_FEAT_ENTITY_CAPS);
	if (n == nullptr)
		return;

	const char *szNode = XmlGetAttr(n, "node");
	const char *szVer = XmlGetAttr(n, "ver");
	const char *szExt = XmlGetAttr(n, "ext");
	if (szNode == nullptr || szVer == nullptr)
		return;

	const char *szHash = XmlGetAttr(n, "hash");
	if (szHash == nullptr) { // old version
		uint8_t hashOut[MIR_SHA1_HASH_SIZE];
		mir_sha1_hash((uint8_t *)szVer, mir_strlen(szVer), hashOut);

		char szHashOut[MIR_SHA1_HASH_SIZE * 2 + 1];
		r->m_pCaps = g_clientCapsManager.GetPartialCaps(szNode, bin2hex(hashOut, _countof(hashOut), szHashOut));
		if (r->m_pCaps == nullptr) {
			r->m_pCaps = g_clientCapsManager.SetClientCaps(szNode, szHashOut, szVer, JABBER_RESOURCE_CAPS_UNINIT);
			RequestOldCapsInfo(r, from);
		}

		MCONTACT hContact = HContactFromJID(from);
		if (hContact)
			UpdateMirVer(hContact, r);
	}
	else {
		r->m_pCaps = g_clientCapsManager.GetPartialCaps(szNode, szVer);
		if (r->m_pCaps == nullptr) {
			r->m_pCaps = g_clientCapsManager.SetClientCaps(szNode, szVer, nullptr, JABBER_RESOURCE_CAPS_UNINIT);
			GetResourceCapabilities(from, r);
		}
	}

	r->m_tszCapsExt = mir_strdup(szExt);
}

void CJabberProto::UpdateJidDbSettings(const char *jid)
{
	JABBER_LIST_ITEM *item = ListGetItemPtr(LIST_ROSTER, jid);
	if (item == nullptr)
		return;

	MCONTACT hContact = HContactFromJID(jid);
	if (hContact == 0)
		return;

	int status = ID_STATUS_OFFLINE;
	if (!item->arResources.getCount()) {
		// set offline only if jid has resources
		if (strchr(jid, '/') == nullptr)
			status = item->getTemp()->m_iStatus;
		if (item->getTemp()->m_szStatusMessage)
			db_set_utf(hContact, "CList", "StatusMsg", item->getTemp()->m_szStatusMessage);
		else
			db_unset(hContact, "CList", "StatusMsg");
	}

	// Determine status to show for the contact based on the remaining resources
	int nSelectedResource = -1;
	int nMaxPriority = -999; // -128...+127 valid range
	for (int i = 0; i < item->arResources.getCount(); i++) {
		pResourceStatus r(item->arResources[i]);
		if (r->m_iPriority > nMaxPriority) {
			nMaxPriority = r->m_iPriority;
			status = r->m_iStatus;
			nSelectedResource = i;
		}
		else if (r->m_iPriority == nMaxPriority) {
			if ((status = JabberCombineStatus(status, r->m_iStatus)) == r->m_iStatus)
				nSelectedResource = i;
		}
	}
	item->getTemp()->m_iStatus = status;
	if (nSelectedResource != -1) {
		pResourceStatus r(item->arResources[nSelectedResource]);
		debugLogA("JabberUpdateJidDbSettings: updating jid %s to rc %s", item->jid, r->m_szResourceName.get());
		if (r->m_szStatusMessage)
			db_set_utf(hContact, "CList", "StatusMsg", r->m_szStatusMessage);
		else
			db_unset(hContact, "CList", "StatusMsg");
		UpdateMirVer(hContact, r);
	}
	else delSetting(hContact, DBSETTING_DISPLAY_UID);

	if (strchr(jid, '@') != nullptr || m_bShowTransport == TRUE)
		if (getWord(hContact, "Status", ID_STATUS_OFFLINE) != status)
			setWord(hContact, "Status", (uint16_t)status);

	// remove x-status icon
	if (status == ID_STATUS_OFFLINE) {
		delSetting(hContact, DBSETTING_XSTATUSID);
		delSetting(hContact, DBSETTING_XSTATUSNAME);
		delSetting(hContact, DBSETTING_XSTATUSMSG);
	}

	MenuUpdateSrmmIcon(item);
}

void CJabberProto::OnProcessPresence(const TiXmlElement *node, ThreadData *info)
{
	if (!node || !node->Name() || mir_strcmp(node->Name(), "presence"))
		return;

	const char *from = XmlGetAttr(node, "from");
	if (from == nullptr)
		return;

	if (m_presenceManager.HandlePresencePermanent(node, info))
		return;

	if (ListGetItemPtr(LIST_CHATROOM, from)) {
		GroupchatProcessPresence(node);
		return;
	}

	MCONTACT hContact;
	bool bSelfPresence = false;
	char szBareFrom[JABBER_MAX_JID_LEN];
	JabberStripJid(from, szBareFrom, _countof(szBareFrom));
	char szBareOurJid[JABBER_MAX_JID_LEN];
	JabberStripJid(info->fullJID, szBareOurJid, _countof(szBareOurJid));

	if (!mir_strcmpi(szBareFrom, szBareOurJid))
		bSelfPresence = true;

	const char *type = XmlGetAttr(node, "type");
	if (type == nullptr || !mir_strcmp(type, "available")) {
		ptrA nick(JabberNickFromJID(from));
		if (nick == nullptr)
			return;

		if ((hContact = HContactFromJID(from)) == 0) {
			if (!mir_strcmpi(info->fullJID, from) || (!bSelfPresence && !ListGetItemPtr(LIST_ROSTER, from))) {
				debugLogA("SKIP Receive presence online from %s (who is not in my roster and not in list - skipping)", from);
				return;
			}
			hContact = DBCreateContact(from, nick, true, false);
		}

		if (!ListGetItemPtr(LIST_ROSTER, from)) {
			debugLogA("Receive presence online from %s (who is not in my roster)", from);
			ListAdd(LIST_ROSTER, from, hContact);
		}

		DBCheckIsTransportedContact(from, hContact);

		int status = ID_STATUS_ONLINE;
		if (auto *show = XmlGetChildText(node, "show")) {
			if (!mir_strcmp(show, "away")) status = ID_STATUS_AWAY;
			else if (!mir_strcmp(show, "xa")) status = ID_STATUS_NA;
			else if (!mir_strcmp(show, "dnd")) status = ID_STATUS_DND;
			else if (!mir_strcmp(show, "chat")) status = ID_STATUS_FREECHAT;
		}

		int idleTime = 0;
		if (auto *idle = XmlGetChildByTag(node, "idle", "xmlns", JABBER_FEAT_IDLE))
			if (auto *szSince = XmlGetAttr(idle, "since"))
				idleTime = str2time(szSince);

		int priority = XmlGetChildInt(node, "priority");
		const char *pszStatus = XmlGetChildText(node, "status");
		ListAddResource(LIST_ROSTER, from, status, pszStatus, priority);

		// XEP-0115: Entity Capabilities
		pResourceStatus r(ResourceInfoFromJID(from));
		if (r != nullptr) {
			if (idleTime)
				r->m_dwIdleStartTime = idleTime;
			OnProcessPresenceCapabilites(node, r);
		}

		UpdateJidDbSettings(from);

		if (strchr(from, '@') == nullptr) {
			UI_SAFE_NOTIFY(m_pDlgServiceDiscovery, WM_JABBER_TRANSPORT_REFRESH);
		}
		debugLogA("%s (%s) online, set contact status to %s", nick.get(), from, T2Utf(Clist_GetStatusModeDescription(status, 0)).get());

		if (m_bEnableAvatars) {
			bool bHasAvatar = false, bRemovedAvatar = false;

			debugLogA("Avatar enabled");
			for (auto *xNode : TiXmlFilter(node, "x")) {
				auto *pszNamespace = XmlGetAttr(xNode, "xmlns");
				if (!mir_strcmp(pszNamespace, "vcard-temp:x:update")) {
					auto *szPhoto = XmlGetChildText(xNode, "photo");
					if (szPhoto && !bHasAvatar) {
						if (mir_strlen(szPhoto)) {
							// no file - no saved hash
							wchar_t tszFileName[MAX_PATH];
							GetAvatarFileName(hContact, tszFileName, _countof(tszFileName));
							if (::_waccess(tszFileName, 0) != 0)
								delSetting(hContact, "AvatarHash");

							bHasAvatar = true;
							ptrA saved(getStringA(hContact, "AvatarHash"));
							if (saved == nullptr || mir_strcmp(saved, szPhoto)) {
								debugLogA("Avatar was changed, reloading");
								SendGetVcard(hContact);
								continue;
							}
						}
						else bRemovedAvatar = true;
					}
				}
				else if (!mir_strcmp(pszNamespace, "miranda:x:vcard")) {
					auto *txt = xNode->GetText();
					if (mir_strlen(txt)) {
						ptrA saved(getStringA(hContact, "VCardHash"));
						if (saved == nullptr || mir_strcmp(saved, txt)) {
							debugLogA("Vcard was changed, let's read it");
							setString(hContact, "VCardHash", txt);
							SendGetVcard(hContact);
						}
					}
				}
			}

			if (!bHasAvatar && bRemovedAvatar) {
				debugLogA("Has no avatar");
				if (!delSetting(hContact, "AvatarHash"))
					ProtoBroadcastAck(hContact, ACKTYPE_AVATAR, ACKRESULT_SUCCESS, nullptr);
			}
		}
		return;
	}

	if (!mir_strcmp(type, "unavailable")) {
		hContact = HContactFromJID(from);
		JABBER_LIST_ITEM *item = ListGetItemPtr(LIST_ROSTER, from);
		if (item != nullptr) {
			ListRemoveResource(LIST_ROSTER, from);

			hContact = HContactFromJID(from);
			if (hContact && !Contact::OnList(hContact)) {
				// remove selfcontact, if where is no more another resources
				if (item->arResources.getCount() == 1 && ResourceInfoFromJID(info->fullJID))
					ListRemoveResource(LIST_ROSTER, info->fullJID);
			}

			// set status only if no more available resources
			if (!item->arResources.getCount()) {
				item->getTemp()->m_iStatus = ID_STATUS_OFFLINE;
				item->getTemp()->m_szStatusMessage = mir_strdup(XmlGetChildText(node, "status"));
			}
		}
		else debugLogA("SKIP Receive presence offline from %s (who is not in my roster)", from);

		UpdateJidDbSettings(from);

		if (strchr(from, '@') == nullptr)
			UI_SAFE_NOTIFY(m_pDlgServiceDiscovery, WM_JABBER_TRANSPORT_REFRESH);

		DBCheckIsTransportedContact(from, hContact);
		return;
	}

	if (!mir_strcmp(type, "subscribe")) {
		if (hContact = HContactFromJID(from))
			AddDbPresenceEvent(hContact, JABBER_DB_EVENT_PRESENCE_SUBSCRIBE);

		ptrA szNick(JabberNickFromJID(from));
		auto *xNick = XmlGetChildByTag(node, "nick", "xmlns", JABBER_FEAT_NICK);
		if (xNick != nullptr) {
			const char *xszNick = xNick->GetText();
			if (xszNick != nullptr && *xszNick) {
				debugLogA("Grabbed nick from presence: %s", xszNick);
				szNick = mir_strdup(xszNick);
			}
		}

		// automatically send authorization allowed to agent/transport
		if (strchr(from, '@') == nullptr || m_bAutoAcceptAuthorization) {
			ListAdd(LIST_ROSTER, from, hContact);
			info->send(XmlNode("presence") << XATTR("to", from) << XATTR("type", "subscribed"));

			if (m_bAutoAdd == TRUE) {
				JABBER_LIST_ITEM *item = ListGetItemPtr(LIST_ROSTER, from);
				if (item == nullptr || (item->subscription != SUB_BOTH && item->subscription != SUB_TO)) {
					debugLogA("Try adding contact automatically jid = %s", from);
					if ((hContact = AddToListByJID(from, 0)) != 0) {
						if (item)
							item->hContact = hContact;
						setUString(hContact, "Nick", szNick);
						Contact::PutOnList(hContact);
					}
				}
			}
		}
		else {
			debugLogA("%s (%s) requests authorization", szNick.get(), from);
			DBAddAuthRequest(from, szNick);
		}
		return;
	}

	if (!mir_strcmp(type, "unsubscribe"))
		if (hContact = HContactFromJID(from))
			AddDbPresenceEvent(hContact, JABBER_DB_EVENT_PRESENCE_UNSUBSCRIBE);

	if (!mir_strcmp(type, "unsubscribed"))
		if (hContact = HContactFromJID(from))
			AddDbPresenceEvent(hContact, JABBER_DB_EVENT_PRESENCE_UNSUBSCRIBED);

	if (!mir_strcmp(type, "error"))
		if (hContact = HContactFromJID(from))
			AddDbPresenceEvent(hContact, JABBER_DB_EVENT_PRESENCE_ERROR);

	if (!mir_strcmp(type, "subscribed")) {
		if (hContact = HContactFromJID(from))
			AddDbPresenceEvent(hContact, JABBER_DB_EVENT_PRESENCE_SUBSCRIBED);

		if (JABBER_LIST_ITEM *item = ListGetItemPtr(LIST_ROSTER, from)) {
			if (item->subscription == SUB_FROM) item->subscription = SUB_BOTH;
			else if (item->subscription == SUB_NONE) {
				item->subscription = SUB_TO;
				if (strchr(from, '@') == nullptr) {
					UI_SAFE_NOTIFY(m_pDlgServiceDiscovery, WM_JABBER_TRANSPORT_REFRESH);
				}
			}
			UpdateSubscriptionInfo(hContact, item);
		}
	}
}

void CJabberProto::OnProcessIq(const TiXmlElement *node)
{
	if (!node->Name() || mir_strcmp(node->Name(), "iq"))
		return;

	const char *type;
	if ((type = XmlGetAttr(node, "type")) == nullptr)
		return;

	int id = JabberGetPacketID(node);

	// new match by id
	if (m_iqManager.HandleIq(id, node))
		return;

	// new iq handler engine
	if (m_iqManager.HandleIqPermanent(node))
		return;

	// RECVED: <iq type='error'> ...
	if (!mir_strcmp(type, "error")) {
		char tszBuf[20];
		_itoa(id, tszBuf, 10);

		debugLogA("XXX on entry");
		// Check for file transfer deny by comparing idStr with ft->iqId
		LISTFOREACH(i, this, LIST_FILE)
		{
			auto *item = ListGetItemPtrFromIndex(i);
			if (item->ft != nullptr && item->ft->state == FT_CONNECTING && !mir_strcmp(tszBuf, item->ft->szId)) {
				debugLogA("Denying file sending request");
				item->ft->state = FT_DENIED;
				if (item->ft->hFileEvent != nullptr)
					SetEvent(item->ft->hFileEvent);	// Simulate the termination of file server connection
			}
		}
	}
	else if ((!mir_strcmp(type, "get") || !mir_strcmp(type, "set"))) {
		XmlNodeIq iq("error", XmlGetAttr(node, "id"), XmlGetAttr(node, "from"));

		auto *pFirstChild = XmlFirstChild(node);
		if (pFirstChild)
			iq += pFirstChild;

		iq << XCHILD("error") << XATTR("type", "cancel")
			<< XCHILDNS("service-unavailable", "urn:ietf:params:xml:ns:xmpp-stanzas");
		m_ThreadInfo->send(iq);
	}
}

void CJabberProto::CancelRegConfig(CJabberFormDlg *, void *)
{
	if (g_pRegInfo)
		g_pRegInfo->conn.SetProgress(100, TranslateT("Registration canceled"));
}

void CJabberProto::SetRegConfig(CJabberFormDlg *pDlg, void *from)
{
	if (g_pRegInfo) {
		iqIdRegSetReg = SerialNext();

		char text[MAX_PATH];
		mir_snprintf(text, "%s@%s", g_pRegInfo->conn.username, g_pRegInfo->conn.server);
		XmlNodeIq iq("set", iqIdRegSetReg, (const char *)from);
		iq << XATTR("from", text);
		TiXmlElement *query = iq << XQUERY(JABBER_FEAT_REGISTER);
		pDlg->GetData(query);
		g_pRegInfo->send(iq);
	}
}

void CJabberProto::OnProcessRegIq(const TiXmlElement *node, ThreadData *info)
{
	if (!node->Name() || mir_strcmp(node->Name(), "iq")) return;
	const char *type = XmlGetAttr(node, "type");
	if (type == nullptr)
		return;

	int id = JabberGetPacketID(node);

	if (!mir_strcmp(type, "result")) {
		auto *queryNode = XmlFirstChild(node, "query");
		if (queryNode != nullptr) {
			const char *str = XmlGetAttr(queryNode, "xmlns");
			if (!mir_strcmp(str, JABBER_FEAT_REGISTER)) {
				auto *xNode = XmlFirstChild(queryNode, "x");
				if (xNode != nullptr) {
					if (!mir_strcmp(XmlGetAttr(xNode, "xmlns"), JABBER_FEAT_DATA_FORMS)) {
						g_pRegInfo = info;

						auto *pDlg = new CJabberFormDlg(this, xNode, LPGEN("Register new user"), &CJabberProto::SetRegConfig, mir_strdup(XmlGetAttr(node, "from")));
						if (info->conn.pDlg)
							pDlg->SetParent(((CDlgBase *)info->conn.pDlg)->GetHwnd());
						pDlg->SetCancel(&CJabberProto::CancelRegConfig);
						pDlg->Display();
						return;
					}
				}
			}
		}

		// RECVED: result of the request for registration mechanism
		// ACTION: send account registration information
		if (id == iqIdRegGetReg) {
			iqIdRegSetReg = SerialNext();

			XmlNodeIq iq("set", iqIdRegSetReg);
			TiXmlElement *query = iq << XQUERY(JABBER_FEAT_REGISTER);
			query << XCHILD("password", info->conn.password);
			query << XCHILD("username", info->conn.username);
			info->send(iq);

			info->conn.SetProgress(75, TranslateT("Sending registration information..."));
		}
		// RECVED: result of the registration process
		// ACTION: account registration successful
		else if (id == iqIdRegSetReg) {
			info->send("</stream:stream>");
			info->conn.SetProgress(100, TranslateT("Registration successful"));
			info->reg_done = true;
		}
	}
	else if (!mir_strcmp(type, "error")) {
		info->conn.SetProgress(100, JabberErrorMsg(node));
		info->reg_done = true;
		info->send("</stream:stream>");
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// ThreadData constructor & destructor

ThreadData::ThreadData(CJabberProto *_pro, JABBER_CONN_DATA *param)
{
	memset(this, 0, sizeof(*this));

	resolveID = -1;
	proto = _pro;
	lastWriteTime = ::GetTickCount();

	if (param != nullptr) {
		bIsReg = true;
		memcpy(&conn, param, sizeof(conn));
	}
}

ThreadData::~ThreadData()
{
	if (!bIsReg && proto->m_ThreadInfo == this)
		proto->m_ThreadInfo = nullptr;

	mir_free(gssapiHostName);
	mir_free(zRecvData);
	mir_free(buffer);
}

void ThreadData::close(void)
{
	if (s) {
		Netlib_CloseHandle(s);
		s = nullptr;
	}
}

void ThreadData::shutdown(void)
{
	bShutdown = true;

	if (s)
		Netlib_Shutdown(s);
}

int ThreadData::recv(char *buf, size_t len)
{
	if (this == nullptr)
		return 0;

	// this select() is still required because shitty openssl is not thread safe
	int nSelRes;
	do {
		NETLIBSELECT nls = {};
		nls.dwTimeout = 500;
		nls.hReadConns[0] = s;
		nSelRes = Netlib_Select(&nls);
		if (nSelRes == SOCKET_ERROR) // error
			return SOCKET_ERROR;
	} while (nSelRes == 0 && !bShutdown);

	if (useZlib)
		return zlibRecv(buf, (long)len);

	return Netlib_Recv(s, buf, (long)len, MSG_DUMPASTEXT);
}

int ThreadData::send(char *buf, int bufsize)
{
	if (this == nullptr)
		return 0;

	if (bShutdown)
		return SOCKET_ERROR;

	lastWriteTime = ::GetTickCount();
	if (bufsize == -1)
		bufsize = (int)mir_strlen(buf);

	mir_cslock lck(proto->m_csSocket);

	int result;
	if (useZlib)
		result = zlibSend(buf, bufsize);
	else
		result = Netlib_Send(s, buf, bufsize, MSG_DUMPASTEXT);

	if (result == SOCKET_ERROR)
		close();

	return result;
}

// Caution: DO NOT use ->send() to send binary (non-string) data
int ThreadData::send(const TiXmlElement *node)
{
	if (this == nullptr || node == nullptr)
		return 0;

	while (auto *parent = node->Parent()) {
		if (parent->ToDocument())
			break;
		node = parent->ToElement();
	}

	int res = send_no_strm_mgmt(node);

	if (res > 0 && proto->m_bEnableStreamMgmt)
		proto->m_StrmMgmt.HandleOutgoingNode(node); //TODO: is this a correct place ?, looks like some nodes does not goes here...

	return res;
}

// this function required for send <r/>, <a/> and more important, for resend stuck nodes by strm_mgmt (xep-0198)
int ThreadData::send_no_strm_mgmt(const TiXmlElement *node)
{
	if (proto->m_sendManager.HandleSendPermanent(node, this))
		return 0;

	proto->OnConsoleProcessXml(node, JCPF_OUT);

	tinyxml2::XMLPrinter printer(0, true);
	node->GetDocument()->Print(&printer);
	CMStringA buf(printer.CStr());

	// strip forbidden control characters from outgoing XML stream
	char *q = buf.GetBuffer();
	for (char *p = buf.GetBuffer(); *p; ++p) {
		int c = *(uint8_t *)p;
		if (c < 32 && c != '\t' && c != '\n' && c != '\r')
			continue;

		*q++ = *p;
	}
	*q = 0;

	return send(buf.GetBuffer(), int(q - buf.GetBuffer()));
}
