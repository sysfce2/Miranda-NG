#include "commonheaders.h"

// generate KeyA pair and return public key
LPSTR InitKeyA(pUinKey ptr, int features)
{
	Sent_NetLog("InitKeyA: %04x", features);

	if (!ptr->cntx)
		ptr->cntx = cpp_create_context(isProtoSmallPackets(ptr->hContact) ? CPP_MODE_BASE64 : 0);

	char *tmp = db_get_sa(ptr->hContact, MODULENAME, "PSK");
	if (tmp) {
		cpp_init_keyp(ptr->cntx, tmp);	// make pre-shared key from password
		mir_free(tmp);
	}

	LPSTR pub_text = cpp_init_keya(ptr->cntx, features);	// calculate public and private key & fill KeyA

	LPSTR keysig;
	if (features & CPP_FEATURES_NEWPG) {
		if (features & KEY_B_SIG)
			keysig = (LPSTR)SIG_KEYB;
		else
			keysig = (LPSTR)SIG_KEYA;
	}
	else if (isProtoSmallPackets(ptr->hContact))
		keysig = (LPSTR)SIG_KEY4;
	else
		keysig = (LPSTR)SIG_KEY3;

	int slen = (int)mir_strlen(keysig);
	int tlen = (int)mir_strlen(pub_text);

	LPSTR keyToSend = (LPSTR)mir_alloc(slen + tlen + 1);

	memcpy(keyToSend, keysig, slen);
	memcpy(keyToSend + slen, pub_text, tlen + 1);

	return keyToSend;
}

// store KeyB into context
int InitKeyB(pUinKey ptr, LPCSTR key)
{
	Sent_NetLog("InitKeyB: %s", key);

	if (!ptr->cntx)
		ptr->cntx = cpp_create_context(isProtoSmallPackets(ptr->hContact) ? CPP_MODE_BASE64 : 0);

	if (!cpp_keyp(ptr->cntx)) {
		char *tmp = db_get_sa(ptr->hContact, MODULENAME, "PSK");
		if (tmp) {
			cpp_init_keyp(ptr->cntx, tmp);	// make pre-shared key from password
			mir_free(tmp);
		}
	}

	cpp_init_keyb(ptr->cntx, key);
	ptr->features = cpp_get_features(ptr->cntx);

	return cpp_get_error(ptr->cntx);
}


// store KeyX into context
void InitKeyX(pUinKey ptr, uint8_t *key)
{
	if (!ptr->cntx)
		ptr->cntx = cpp_create_context(isProtoSmallPackets(ptr->hContact) ? CPP_MODE_BASE64 : 0);

	cpp_set_keyx(ptr->cntx, key);
}


// calculate secret key
BOOL CalculateKeyX(pUinKey ptr, MCONTACT hContact)
{
	int agr = cpp_calc_keyx(ptr->cntx);
	if (agr) {
		// do this only if key exchanged is ok
		// we use a 192bit key
		int keysize = cpp_size_keyx();
		uint8_t *buffer = (uint8_t*)alloca(keysize); // buffer for hash

		// store key
		cpp_get_keyx(ptr->cntx, buffer);

		// store key in database
		db_set_blob(hContact, MODULENAME, "offlineKey", buffer, keysize);

		// store timeout of key in database (2 days)
		g_plugin.setDword(hContact, "offlineKeyTimeout", gettime() + (60 * 60 * 24 * g_plugin.getWord("okt", 2)));

		// key exchange is finished
		showPopupEC(ptr->hContact);
	}
	else // agree value problem
		showPopup(LPGEN("Key exchange failed..."), hContact, g_hPOP[POP_PU_DIS], 0);

	return agr != 0;
}


// encrypt message
LPSTR encrypt(pUinKey ptr, LPCSTR szEncMsg)
{
	LPSTR szSig = (LPSTR)(ptr->offlineKey ? SIG_ENOF : SIG_ENON);

	int slen = (int)mir_strlen(szSig);
	int clen = (int)mir_strlen(szEncMsg);

	LPSTR szMsg = (LPSTR)mir_alloc(clen + slen + 1);
	memcpy(szMsg, szSig, slen);
	memcpy(szMsg + slen, szEncMsg, clen + 1);

	return szMsg;
}


// encode message
LPSTR encodeMsg(pUinKey ptr, LPARAM lParam)
{
	CCSDATA *pccsd = (CCSDATA *)lParam;
	return encrypt(ptr, cpp_encodeU(ptr->cntx, (LPSTR)pccsd->lParam));
}


// decode message
LPSTR decodeMsg(pUinKey ptr, LPARAM lParam, LPSTR szEncMsg)
{
	CCSDATA *pccsd = (CCSDATA *)lParam;
	auto *dbei = (DB::EventInfo *)pccsd->lParam;

	LPSTR szNewMsg = nullptr;
	LPSTR szOldMsg = cpp_decodeU(ptr->cntx, szEncMsg);

	if (szOldMsg == nullptr) {
		ptr->decoded = false;
		switch (cpp_get_error(ptr->cntx)) {
		case CPP_ERROR_BAD_LEN:
			szNewMsg = mir_strdup(Translate("SecureIM: Error while decrypting the message, bad message length."));
			break;
		case CPP_ERROR_BAD_CRC:
			szNewMsg = mir_strdup(Translate("SecureIM: Error while decrypting the message, bad message CRC."));
			break;
		default:
			ptr->decoded = true;
			szNewMsg = mir_strdup(Translate("SecureIM: Error while decrypting the message."));
			break;
		}
	}
	else {
		ptr->decoded = true;
		int olen = (int)mir_strlen(szOldMsg) + 1;
		szNewMsg = (LPSTR)mir_alloc(olen);
		memcpy(szNewMsg, szOldMsg, olen);
	}
	dbei->pBlob = szNewMsg;
	return szNewMsg;
}


BOOL LoadKeyPGP(pUinKey ptr)
{
	int mode = db_get_b(ptr->hContact, MODULENAME, "pgp_mode", 255);
	if (mode == 0) {
		DBVARIANT dbv;
		if(!db_get(ptr->hContact, MODULENAME, "pgp", &dbv)) {
			BOOL r = (dbv.type == DBVT_BLOB);
			if (r)
				pgp_set_keyid(ptr->cntx, (PVOID)dbv.pbVal);
			db_free(&dbv);
			return r;
		}
	}
	else if (mode == 1) {
		LPSTR key = db_get_sa(ptr->hContact, MODULENAME, "pgp");
		if (key) {
			pgp_set_key(ptr->cntx, key);
			mir_free(key);
			return 1;
		}
	}
	return 0;
}

BOOL LoadKeyGPG(pUinKey ptr)
{
	LPSTR key = db_get_sa(ptr->hContact, MODULENAME, "gpg");
	if (key) {
		gpg_set_keyid(ptr->cntx, key);
		mir_free(key);
		return 2;
	}
	return 0;
}
