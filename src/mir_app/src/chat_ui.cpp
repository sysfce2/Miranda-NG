/*
Chat module plugin for Miranda IM

Copyright 2000-12 Miranda IM, 2012-23 Miranda NG team,
all portions of this codebase are copyrighted to the people
listed in contributors.txt.

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

#include "chat.h"

CMOption<bool> g_bChatPopupInactive(CHAT_MODULE, "PopupInactiveOnly", true);
CMOption<bool> g_bChatTrayInactive(CHAT_MODULE, "TrayIconInactiveOnly", true);

/////////////////////////////////////////////////////////////////////////////////////////
// Group chat - Events

#define NR_GC_EVENTS 12

static UINT _eventorder[] =
{
	GC_EVENT_ACTION,
	GC_EVENT_MESSAGE,
	GC_EVENT_NICK,
	GC_EVENT_JOIN,
	GC_EVENT_PART,
	GC_EVENT_TOPIC,
	GC_EVENT_ADDSTATUS,
	GC_EVENT_INFORMATION,
	GC_EVENT_QUIT,
	GC_EVENT_KICK,
	GC_EVENT_NOTICE,
	GC_EVENT_HIGHLIGHT
};

class CChatEventOptionDlg : public CDlgBase
{
	CCtrlCheck chkTray, chkPopup, chkRightClick;
	CCtrlMButton btn1, btn2, btn3, btn4, btn5;

	void InvertColumn(int ctrlId)
	{
		int enabled = !IsDlgButtonChecked(m_hwnd, ctrlId);
		for (int i = 0; i < _countof(_eventorder); i++)
			CheckDlgButton(m_hwnd, ctrlId + i, enabled);
		NotifyChange();
	}

public:
	CChatEventOptionDlg() :
		CDlgBase(g_plugin, IDD_OPT_CHAT_EVENTS),
		chkTray(this, IDC_TRAYONLYFORINACTIVE),
		chkPopup(this, IDC_POPUPONLYFORINACTIVE),
		chkRightClick(this, IDC_RIGHTCLICK),
		btn1(this, IDC_ICON1, SKINICON_OTHER_WINDOW, LPGEN("Window")),
		btn2(this, IDC_ICON2, SKINICON_OTHER_POPUP, LPGEN("Popup")),
		btn3(this, IDC_ICON3, SKINICON_OTHER_MIRANDA, LPGEN("Tray")),
		btn4(this, IDC_ICON4, SKINICON_OTHER_SOUND, LPGEN("Sound")),
		btn5(this, IDC_ICON5, SKINICON_EVENT_FILE, LPGEN("Log to file"))
	{
		CreateLink(chkTray, g_bChatTrayInactive);
		CreateLink(chkPopup, g_bChatPopupInactive);
		CreateLink(chkRightClick, g_chatApi.bRightClickFilter);

		btn1.OnClick = Callback(this, &CChatEventOptionDlg::onClick_Window);
		btn2.OnClick = Callback(this, &CChatEventOptionDlg::onClick_Popup);
		btn3.OnClick = Callback(this, &CChatEventOptionDlg::onClick_Tray);
		btn4.OnClick = Callback(this, &CChatEventOptionDlg::onClick_Sound);
		btn5.OnClick = Callback(this, &CChatEventOptionDlg::onClick_Log);
	}

	bool OnInitDialog() override
	{
		btn1.MakeFlat(); btn2.MakeFlat(); btn3.MakeFlat(); btn4.MakeFlat();

		for (int i = 0; i < _countof(_eventorder); i++) {
			if (_eventorder[i] != GC_EVENT_HIGHLIGHT) {
				CheckDlgButton(m_hwnd, IDC_1 + i, Chat::iFilterFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED);
				CheckDlgButton(m_hwnd, IDC_L1 + i, Chat::iDiskLogFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED);
			}
			CheckDlgButton(m_hwnd, IDC_P1 + i, Chat::iPopupFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(m_hwnd, IDC_S1 + i, Chat::iSoundFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(m_hwnd, IDC_T1 + i, Chat::iTrayIconFlags & _eventorder[i] ? BST_CHECKED : BST_UNCHECKED);
		}
		return true;
	}

	bool OnApply() override
	{
		uint32_t dwFilterFlags = 0, dwTrayFlags = 0, dwPopupFlags = 0, dwSoundFlags = 0, dwLogFlags = 0;

		for (int i = 0; i < _countof(_eventorder); i++) {
			if (_eventorder[i] != GC_EVENT_HIGHLIGHT) {
				dwFilterFlags |= (IsDlgButtonChecked(m_hwnd, IDC_1 + i) ? _eventorder[i] : 0);
				dwLogFlags |= (IsDlgButtonChecked(m_hwnd, IDC_L1 + i) ? _eventorder[i] : 0);
			}
			dwSoundFlags |= (IsDlgButtonChecked(m_hwnd, IDC_S1 + i) ? _eventorder[i] : 0);
			dwPopupFlags |= (IsDlgButtonChecked(m_hwnd, IDC_P1 + i) ? _eventorder[i] : 0);
			dwTrayFlags |= (IsDlgButtonChecked(m_hwnd, IDC_T1 + i) ? _eventorder[i] : 0);
		}

		Chat::iPopupFlags = dwPopupFlags;
		Chat::iSoundFlags = dwSoundFlags;
		Chat::iFilterFlags = dwFilterFlags;
		Chat::iTrayIconFlags = dwTrayFlags;
		Chat::iDiskLogFlags = dwLogFlags;

		LoadGlobalSettings();
		return true;
	}

	void onClick_Window(CCtrlButton *) { InvertColumn(IDC_1); }
	void onClick_Popup(CCtrlButton *) { InvertColumn(IDC_P1); }
	void onClick_Sound(CCtrlButton *) { InvertColumn(IDC_S1); }
	void onClick_Tray(CCtrlButton *) { InvertColumn(IDC_T1); }
	void onClick_Log(CCtrlButton *) { InvertColumn(IDC_L1); }
};

/////////////////////////////////////////////////////////////////////////////////////////
// Popup options

class COptPopupDlg : public CDlgBase
{
	CCtrlSpin  spinTimeout;
	CCtrlCheck chkRadio1, chkRadio2, chkRadio3;
	CCtrlColor clrBack, clrText;

public:
	COptPopupDlg() :
		CDlgBase(g_plugin, IDD_OPTIONSPOPUP),
		clrBack(this, IDC_BKG),
		clrText(this, IDC_TEXT),
		chkRadio1(this, IDC_RADIO1),
		chkRadio2(this, IDC_RADIO2),
		chkRadio3(this, IDC_RADIO3),
		spinTimeout(this, IDC_SPIN1, 100, -1)
	{
		chkRadio1.OnChange = chkRadio2.OnChange = chkRadio3.OnChange = Callback(this, &COptPopupDlg::onChange_Radio);
	}

	bool OnInitDialog() override
	{
		clrBack.SetColor(g_Settings->crPUBkgColour);
		clrText.SetColor(g_Settings->crPUTextColour);

		if (g_Settings->iPopupStyle == 2)
			CheckDlgButton(m_hwnd, IDC_RADIO2, BST_CHECKED);
		else if (g_Settings->iPopupStyle == 3)
			CheckDlgButton(m_hwnd, IDC_RADIO3, BST_CHECKED);
		else
			CheckDlgButton(m_hwnd, IDC_RADIO1, BST_CHECKED);
		onChange_Radio(0);

		spinTimeout.SetPosition(g_Settings->iPopupTimeout);
		return true;
	}

	bool OnApply() override
	{
		if (IsDlgButtonChecked(m_hwnd, IDC_RADIO2) == BST_CHECKED)
			g_Settings->iPopupStyle = 2;
		else if (IsDlgButtonChecked(m_hwnd, IDC_RADIO3) == BST_CHECKED)
			g_Settings->iPopupStyle = 3;
		else
			g_Settings->iPopupStyle = 1;
		db_set_b(0, CHAT_MODULE, "PopupStyle", g_Settings->iPopupStyle);

		db_set_w(0, CHAT_MODULE, "PopupTimeout", g_Settings->iPopupTimeout = spinTimeout.GetPosition());
		db_set_dw(0, CHAT_MODULE, "PopupColorBG", g_Settings->crPUBkgColour = clrBack.GetColor());
		db_set_dw(0, CHAT_MODULE, "PopupColorText", g_Settings->crPUTextColour = clrText.GetColor());
		return true;
	}

	void onChange_Radio(CCtrlCheck *)
	{
		bool bStatus = chkRadio3.GetState();
		clrBack.Enable(bStatus);
		clrText.Enable(bStatus);
	}
};

/////////////////////////////////////////////////////////////////////////////////////////

void ChatOptionsInit(WPARAM wParam)
{
	OPTIONSDIALOGPAGE odp = {};
	odp.flags = ODPF_BOLDGROUPS;
	odp.position = 910000000;
	odp.szGroup.a = LPGEN("Message sessions");
	odp.szTitle.a = LPGEN("Group chats");
	odp.szTab.a = LPGEN("Events and filters");
	odp.pDialog = new CChatEventOptionDlg();
	g_plugin.addOptions(wParam, &odp);

	odp.position = 910000002;
	odp.szTitle.a = LPGEN("Group chats");
	odp.szGroup.a = LPGEN("Popups");
	odp.szTab.a = nullptr;
	odp.pDialog = new COptPopupDlg();
	g_plugin.addOptions(wParam, &odp);
}
