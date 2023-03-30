/*
Copyright (C) 2012-23 Miranda NG team (https://miranda-ng.org)

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

#include "../../../../miranda-private-keys/Telegram/api.h"

///////////////////////////////////////////////////////////////////////////////

INT_PTR CALLBACK CTelegramProto::EnterEmail(void *param)
{
	auto *ppro = (CTelegramProto *)param;

	ENTER_STRING es = {};
	es.szModuleName = ppro->m_szModuleName;
	es.caption = TranslateT("Enter email address for account verification");
	if (EnterString(&es)) {
		ppro->SendQuery(new TD::setAuthenticationEmailAddress(T2Utf(es.ptszResult).get()), &CTelegramProto::OnUpdateAuth);
		mir_free(es.ptszResult);
	}
	else ppro->LogOut();
	return 0;
}

INT_PTR CALLBACK CTelegramProto::EnterEmailCode(void *param)
{
	auto *ppro = (CTelegramProto *)param;

	ENTER_STRING es = {};
	es.szModuleName = ppro->m_szModuleName;
	es.caption = TranslateT("Enter verification code received by email");
	if (EnterString(&es)) {
		ppro->SendQuery(new TD::checkAuthenticationEmailCode(
			TD::object_ptr<TD::EmailAddressAuthentication>(new TD::emailAddressAuthenticationCode(T2Utf(es.ptszResult).get()))),
			&CTelegramProto::OnUpdateAuth);
		mir_free(es.ptszResult);
	}
	else ppro->LogOut();
	return 0;
}

INT_PTR CALLBACK CTelegramProto::EnterPhoneCode(void *param)
{
	auto *ppro = (CTelegramProto *)param;

	ENTER_STRING es = {};
	es.szModuleName = ppro->m_szModuleName;
	es.caption = TranslateT("Enter the secret code sent to another device");
	if (EnterString(&es)) {
		ppro->SendQuery(new TD::checkAuthenticationCode(T2Utf(es.ptszResult).get()), &CTelegramProto::OnUpdateAuth);
		mir_free(es.ptszResult);
	}
	else ppro->LogOut();
	return 0;
}

INT_PTR CALLBACK CTelegramProto::EnterPassword(void *param)
{
	auto *ppro = (CTelegramProto *)param;
	CMStringW wszTitle(TranslateT("Enter password"));

	auto *pAuth = (TD::authorizationStateWaitPassword *)ppro->pAuthState.get();
	if (!pAuth->password_hint_.empty())
		wszTitle.AppendFormat(TranslateT(" (hint: %s)"), Utf2T(pAuth->password_hint_.c_str()).get());

	ENTER_STRING es = {};
	es.szModuleName = ppro->m_szModuleName;
	es.caption = wszTitle;
	es.type = ESF_PASSWORD;
	if (EnterString(&es)) {
		ppro->SendQuery(new TD::checkAuthenticationPassword(T2Utf(es.ptszResult).get()), &CTelegramProto::OnUpdateAuth);
		mir_free(es.ptszResult);
	}
	else ppro->LogOut();
	return 0;
}

void CTelegramProto::ProcessAuth(TD::updateAuthorizationState *pObj)
{
	pAuthState = std::move(pObj->authorization_state_);
	switch (pAuthState->get_id()) {
	case TD::authorizationStateReady::ID:
		if (pConnState && pConnState->get_id() == TD::connectionStateReady::ID)
			OnLoggedIn();
		break;

	case TD::authorizationStateWaitTdlibParameters::ID:
		if (!m_bUnregister) {
			MFileVersion v;
			char text[100];
			Miranda_GetFileVersion(&v);
			mir_snprintf(text, "%d.%d.%d.%d", v[0], v[1], v[2], v[3]);

			CMStringW wszPath(GetProtoFolder());

			auto *request = new TD::setTdlibParameters();
			request->database_directory_ = T2Utf(wszPath).get();
			request->use_message_database_ = false;
			request->use_secret_chats_ = true;
			request->api_id_ = MIRANDA_API_ID;
			request->api_hash_ = MIRANDA_API_HASH;
			request->system_language_code_ = "en";
			request->device_model_ = T2Utf(m_wszDeviceName).get();
			request->application_version_ = text;
			request->enable_storage_optimizer_ = true;
			SendQuery(request, &CTelegramProto::OnUpdateAuth);
		}
		else LogOut();
		break;

	case TD::authorizationStateWaitPhoneNumber::ID:
		SendQuery(new TD::setAuthenticationPhoneNumber(m_szFullPhone.c_str(), nullptr), &CTelegramProto::OnUpdateAuth);
		break;

	case TD::authorizationStateWaitCode::ID:
		CallFunctionSync(EnterPhoneCode, this);
		break;

	case TD::authorizationStateWaitPassword::ID:
		CallFunctionSync(EnterPassword, this);
		break;

	case TD::authorizationStateWaitEmailAddress::ID:
		CallFunctionSync(EnterEmail, this);
		break;

	case TD::authorizationStateWaitEmailCode::ID:
		CallFunctionSync(EnterEmailCode, this);
		break;

	case TD::authorizationStateClosed::ID:
		debugLogA("Connection terminated, exiting");
		LogOut();
		break;
	}
}

void CTelegramProto::OnUpdateAuth(td::ClientManager::Response &response)
{
	if (response.object->get_id() == TD::error::ID) {
		auto *pError = (TD::error *)response.object.get();
		debugLogA("error happened: %s", to_string(*pError).c_str());

		if (pError->message_ == "PHONE_CODE_EXPIRED")
			Popup(0, TranslateT("Secret code expired"), TranslateT("Error"));
		else if (pError->message_ == "INVALID_PHONE_CODE")
			Popup(0, TranslateT("Invalid secret code"), TranslateT("Error"));
		else if (pError->message_ == "PASSWORD_HASH_INVALID")
			Popup(0, TranslateT("Invalid password"), TranslateT("Error"));

		pAuthState = std::move(nullptr);
		LogOut();
	}
}
