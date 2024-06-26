/*
Copyright (C) 2012-18 Miranda NG team (https://miranda-ng.org)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2
of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"

COptionsPageDlg::COptionsPageDlg() :
	CSuper(IDD_OPTPAGE),
	edtFile(this, IDC_FILENAME),
	btnFile(this, IDC_OPENFILE),
	chkFixRtf(this, IDC_FIX_RTF),
	chkFixUtf(this, IDC_FIX_UTF8),
	chkMarkRead(this, IDC_MARKREAD),
	chkRemoveDups(this, IDC_REMOVE_DUPS),
	chkFixServerIds(this, IDC_FIX_SERVER_IDS)
{
}

bool COptionsPageDlg::OnInitDialog()
{
	CSuper::OnInitDialog();

	auto *opts = getOpts();
	edtFile.SetText(opts->filename);

	chkFixRtf.SetState(opts->bCheckRtf);
	chkFixUtf.SetState(opts->bCheckUtf);
	chkMarkRead.SetState(opts->bMarkRead);
	chkRemoveDups.SetState(opts->bCheckDups);
	chkFixServerIds.SetState(opts->bCheckServerIds);

	if (opts->dbChecker != nullptr) {
		edtFile.Disable();
		btnFile.Disable();
	}
	return true;
}

int COptionsPageDlg::Resizer(UTILRESIZECONTROL *urc)
{
	switch (urc->wId) {
	case IDC_FILENAME:
	case IDC_MARKREAD:
	case IDC_SPLITTER:
		return RD_ANCHORX_WIDTH | RD_ANCHORY_TOP;

	case IDC_OPENFILE:
		return RD_ANCHORX_RIGHT | RD_ANCHORY_TOP;
	}

	return RD_ANCHORX_LEFT | RD_ANCHORY_TOP;
}

void COptionsPageDlg::OnNext()
{
	wchar_t tszMsg[1024];

	auto *opts = getOpts();
	if (opts->dbChecker == nullptr) {
		DATABASELINK *dblink = FindDatabasePlugin(opts->filename);
		if (dblink == nullptr) {
			mir_snwprintf(tszMsg,
				TranslateT("Database Checker cannot find a suitable database plugin to open '%s'."),
				opts->filename);
LBL_Error:
			MessageBox(m_hwnd, tszMsg, TranslateT("Error"), MB_OK | MB_ICONERROR);
			return;
		}

		auto *pDb = dblink->Load(opts->filename, false);
		if (pDb == nullptr) {
			changePage(new COpenErrorDlg());
			return;
		}

		opts->dbChecker = pDb->GetChecker();
		if (opts->dbChecker == nullptr) {
			mir_snwprintf(tszMsg, TranslateT("Database driver '%s' doesn't support checking."), TranslateW(dblink->szFullName));
			goto LBL_Error;
		}

		opts->db = pDb;
	}

	opts->bCheckRtf = chkFixRtf.IsChecked();
	opts->bCheckUtf = chkFixUtf.IsChecked();
	opts->bMarkRead = chkMarkRead.IsChecked();
	opts->bCheckDups = chkRemoveDups.IsChecked();
	opts->bCheckServerIds = chkFixServerIds.IsChecked();

	changePage(new CProgressPageDlg());
}
