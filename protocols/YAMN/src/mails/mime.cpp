/*
 * This code implements retrieving info from MIME header
 *
 * (c) majvan 2002-2004
 */

#include "../stdafx.h"

/////////////////////////////////////////////////////////////////////////////////////////
// Makes a string lowercase
// string- string to be lowercased

void inline ToLower(char *string)
{
	for (; *string != 0; string++)
		if (*string >= 'A' && *string <= 'Z')
			*string = *string - 'A' + 'a';
}

/////////////////////////////////////////////////////////////////////////////////////////
// Copies one string to another
// srcstart- source string
// srcend- address to the end of source string
// dest- pointer that stores new allocated string that contains copy of source string
// mode- MIME_PLAIN or MIME_MAIL (MIME_MAIL deletes '"' characters (or '<' and '>') if they are at start and end of source string

static void CopyToHeader(char *srcstart, char *srcend, char **dest, int mode)
{
	if (dest == nullptr)
		return;
	if (srcstart >= srcend)
		return;

	if ((mode == MIME_MAIL) && (((*srcstart == '"') && (*(srcend - 1) == '"')) || ((*srcstart == '<') && (*(srcend - 1) == '>')))) {
		srcstart++;
		srcend--;
	}

	if (srcstart >= srcend)
		return;

	if (nullptr != *dest)
		delete[] * dest;
	if (nullptr == (*dest = new char[srcend - srcstart + 1]))
		return;

	char *dst = *dest;

	for (; srcstart < srcend; dst++, srcstart++) {
		if (ENDLINE(srcstart)) {
			while (ENDLINE(srcstart) || WS(srcstart)) srcstart++;
			*dst = ' ';
			srcstart--;		// because at the end of "for loop" we increment srcstart
		}
		else *dst = *srcstart;
	}
	*dst = 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Extracts email address (finds nick name and mail and then stores them to strings)
// finder- source string
// storeto- pointer that receives address of mail string
// storetonick- pointer that receives address of nickname

static void ExtractAddressFromLine(char *finder, char **storeto, char **storetonick)
{
	if (finder == nullptr) {
		*storeto = *storetonick = nullptr;
		return;
	}
	
	SkipSpaces(finder);
	if ((*finder) != '<') {
		char *finderend = finder + 1;
		do {
			if (ENDLINEWS(finderend))						// after endline information continues
				finderend += 2;
			while (!ENDLINE(finderend) && !EOS(finderend)) finderend++;		// seek to the end of line or to the end of string
		} while (ENDLINEWS(finderend));
		finderend--;
		while (WS(finderend) || ENDLINE(finderend)) finderend--;				// find the end of text, no whitespace
		if (*finderend != '>')						// not '>' at the end of line
			CopyToHeader(finder, finderend + 1, storeto, MIME_MAIL);
		else { // at the end of line, there's '>'
			char *finder2 = finderend;
			while ((*finder2 != '<') && (finder2 > finder)) finder2--;		// go to matching '<' or to the start
			CopyToHeader(finder2, finderend + 1, storeto, MIME_MAIL);
			if (*finder2 == '<')						// if we found '<', the rest copy as from nick
			{
				finder2--;
				while (WS(finder2) || ENDLINE(finder2)) finder2--;		// parse whitespace
				CopyToHeader(finder, finder2 + 1, storetonick, MIME_MAIL);		// and store nickname
			}
		}
	}
	else {
		char *finderend = finder + 1;
		do {
			if (ENDLINEWS(finderend)) // after endline information continues
				finderend += 2;

			// seek to the matching < or to the end of line or to the end of string
			while (!ENDLINE(finderend) && (*finderend != '>') && !EOS(finderend))
				finderend++;
		} while (ENDLINEWS(finderend));

		CopyToHeader(finder, finderend + 1, storeto, MIME_MAIL); // go to first '>' or to the end and copy
		finder = finderend + 1;
		SkipSpaces(finder);
		if (!ENDLINE(finder) && !EOS(finder)) { // if there are chars yet, it's nick
			finderend = finder + 1;
			while (!ENDLINE(finderend) && !EOS(finderend)) finderend++;	// seek to the end of line or to the end of string
			finderend--;
			while (WS(finderend)) // find the end of line, no whitespace
				finderend--;
			CopyToHeader(finder, finderend + 1, storetonick, MIME_MAIL);
		}
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// Extracts simple text from string
// finder- source string
// storeto- pointer that receives address of string

static void ExtractStringFromLine(char *finder, char **storeto)
{
	if (finder == nullptr) {
		*storeto = nullptr;
		return;
	}
	
	SkipSpaces(finder);
	char *finderend = finder;

	do {
		if (ENDLINEWS(finderend)) finderend++;						// after endline information continues
		while (!ENDLINE(finderend) && !EOS(finderend)) finderend++;
	} while (ENDLINEWS(finderend));
	finderend--;
	while (WS(finderend)) // find the end of line, no whitespace
		finderend--;
	CopyToHeader(finder, finderend + 1, storeto, MIME_PLAIN);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Extracts some item from content-type string
// Example: ContentType string: "TEXT/PLAIN; charset=US-ASCII", item:"charset=", returns: "US-ASCII"
// ContetType- content-type string
// value- string item
// returns extracted string (or NULL when not found)

char* ExtractFromContentType(char *ContentType, char *value)
{
	char *lowered = _strdup(ContentType);
	ToLower(lowered);
	char *finder = strstr(lowered, value);
	if (finder == nullptr) {
		free(lowered);
		return nullptr;
	}
	finder = finder - lowered + ContentType;
	free(lowered);

	char *temp = finder - 1;
	while ((temp > ContentType) && WS(temp)) // now we have to find, if the word "Charset=" is located after ';' like "; Charset="
		temp--; 
	if (*temp != ';' && !ENDLINE(temp) && temp != ContentType)
		return nullptr;
	
	finder = finder + mir_strlen(value); // jump over value string

	SkipSpaces(finder);
	temp = finder;
	while (*temp != 0 && *temp != ';') // jump to the end of setting (to the next ;)
		temp++;
	temp--;
	while (WS(temp)) // remove whitespaces from the end
		temp--;
	if (*finder == '\"') { // remove heading and tailing quotes
		finder++;
		if (*temp == '\"')
			temp--;
	}

	char *CopiedString = new char[++temp - finder + 1];
	
	char *copier;
	for (copier = CopiedString; finder != temp; *copier++ = *finder++);
	*copier = 0; // and end it with zero character

	return CopiedString;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Extracts info from header text into header members
// Note that this function as well as CShortHeadwer can be always changed, because there are many items to extract
// (e.g. the X-Priority and Importance and so on)
// items- translated header (see TranslateHeaderFcn)
// head- header to be filled with values extracted from items

void ExtractShortHeader(CMimeItem *items, CShortHeader *head)
{
	for (; items != nullptr; items = items->Next) {
		// at the start of line
		// MessageBox(NULL,items->value,items->name,0);
		if (0 == _strnicmp(items->name, "From", 4)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting from>");
			#endif
			ExtractAddressFromLine(items->value, &head->From, &head->FromNick);
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}
		else if (0 == _strnicmp(items->name, "Return-Path", 11)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting return-path>");
			#endif
			ExtractAddressFromLine(items->value, &head->ReturnPath, &head->ReturnPathNick);
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}
		else if (0 == _strnicmp(items->name, "Subject", 7)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting subject>");
			#endif
			ExtractStringFromLine(items->value, &head->Subject);
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}
		else if (0 == _strnicmp(items->name, "Body", 4)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting body>");
			#endif
			ExtractStringFromLine(items->value, &head->Body);
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}
		else if (0 == _strnicmp(items->name, "Date", 4)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting date>");
			#endif
			ExtractStringFromLine(items->value, &head->Date);
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}
		else if (0 == _strnicmp(items->name, "Content-Type", 12)) {
			if (items->value == nullptr)
				continue;

			char *ContentType = nullptr, *CharSetStr;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting Content-Type>");
			#endif
			ExtractStringFromLine(items->value, &ContentType);
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
			ToLower(ContentType);
			if (nullptr != (CharSetStr = ExtractFromContentType(ContentType, "charset="))) {
				head->CP = GetCharsetFromString(CharSetStr, mir_strlen(CharSetStr));
				delete[] CharSetStr;
			}
			delete[] ContentType;
		}
		else if (0 == _strnicmp(items->name, "Importance", 10)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<Extracting importance>");
			#endif
			if (head->Priority != -1) {
				if (0 == strncmp(items->value, "low", 3))
					head->Priority = 5;
				else if (0 == strncmp(items->value, "normal", 6))
					head->Priority = 3;
				else if (0 == strncmp(items->value, "high", 4))
					head->Priority = 1;
			}
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}
		else if (0 == _strnicmp(items->name, "X-Priority", 10)) {
			if (items->value == nullptr)
				continue;
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "<X-Priority>");
			#endif
			if ((*items->value >= '1') && (*items->value <= '5'))
				head->Priority = *items->value - '0';
			#ifdef DEBUG_DECODE
			mir_writeLogA(DecodeFile, "</Extracting>\n");
			#endif
		}

	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// Deletes list of YAMN_MIMENAMES structures
// Names- pointer to first item of list

static void DeleteNames(CMimeNames *Names)
{
	CMimeNames *Parser = Names;
	for (; Parser != nullptr; Parser = Parser->Next) {
		if (Parser->Value != nullptr)
			delete[] Parser->Value;
		if (Parser->ValueNick != nullptr)
			delete[] Parser->ValueNick;

		CMimeNames *Old = Parser;
		Parser = Parser->Next;
		delete Old;
	}
}

CHeader::~CHeader()
{
	DeleteNames(To);
	DeleteNames(Cc);
	DeleteNames(Bcc);
}

/////////////////////////////////////////////////////////////////////////////////////////
// Deletes list of YAMN_MIMESHORTNAMES structures
// Names- pointer to first item of list

static void DeleteShortNames(CShortNames *Names)
{
	CShortNames *Parser = Names;
	for (; Parser != nullptr; Parser = Parser->Next) {
		if (Parser->Value != nullptr)
			delete[] Parser->Value;
		if (Parser->ValueNick != nullptr)
			delete[] Parser->ValueNick;

		CShortNames *Old = Parser;
		Parser = Parser->Next;
		delete Old;
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// Deletes items in CShortHeader structure
// head- structure whose items are deleted

void DeleteShortHeaderContent(CShortHeader *head)
{
	if (head->From != nullptr) delete[] head->From;
	if (head->FromNick != nullptr) delete[] head->FromNick;
	if (head->ReturnPath != nullptr) delete[] head->ReturnPath;
	if (head->ReturnPathNick != nullptr) delete[] head->ReturnPathNick;
	if (head->Subject != nullptr) delete[] head->Subject;
	if (head->Date != nullptr) delete[] head->Date;
	if (head->To != nullptr) DeleteShortNames(head->To);
	if (head->Cc != nullptr) DeleteShortNames(head->Cc);
	if (head->Bcc != nullptr) DeleteShortNames(head->Bcc);
	if (head->Body != nullptr) delete[] head->Body;
}

/////////////////////////////////////////////////////////////////////////////////////////
// Extracts header to mail using ExtractShortHeader fcn.
// items- translated header (see TranslateHeaderFcn)
// CP- codepage used when no default found
// head- header to be filled with values extracted from items, in unicode (wide char)

void ExtractHeader(CMimeItem *items, int &CP, CHeader *head)
{
	CShortHeader ShortHeader = {};
	ShortHeader.Priority = ShortHeader.CP = -1;
	#ifdef DEBUG_DECODE
	mir_writeLogA(DecodeFile, "<Extracting header>\n");
	#endif
	ExtractShortHeader(items, &ShortHeader);

	head->Priority = ShortHeader.Priority == -1 ? 3 : ShortHeader.Priority;
	CP = ShortHeader.CP == -1 ? CP : ShortHeader.CP;
	#ifdef DEBUG_DECODE
	if (NULL != ShortHeader.From)
		mir_writeLogA(DecodeFile, "<Decoded from>%s</Decoded)\n", ShortHeader.From);
	if (NULL != ShortHeader.FromNick)
		mir_writeLogA(DecodeFile, "<Decoded from-nick>%s</Decoded)\n", ShortHeader.FromNick);
	if (NULL != ShortHeader.ReturnPath)
		mir_writeLogA(DecodeFile, "<Decoded return-path>%s</Decoded)\n", ShortHeader.ReturnPath);
	if (NULL != ShortHeader.ReturnPathNick)
		mir_writeLogA(DecodeFile, "<Decoded return-path nick>%s</Decoded)\n", ShortHeader.ReturnPathNick);
	if (NULL != ShortHeader.Subject)
		mir_writeLogA(DecodeFile, "<Decoded subject>%s</Decoded)\n", ShortHeader.Subject);
	if (NULL != ShortHeader.Date)
		mir_writeLogA(DecodeFile, "<Decoded date>%s</Decoded)\n", ShortHeader.Date);
	mir_writeLogA(DecodeFile, "</Extracting header>\n");
	mir_writeLogA(DecodeFile, "<Convert>\n");
	#endif

	head->wszFrom = ConvertCodedStringToUnicode(ShortHeader.From, CP, MIME_PLAIN);

	#ifdef DEBUG_DECODE
	if (NULL != head->From)
		mir_writeLogW(DecodeFile, L"<Converted from>%s</Converted>\n", head->From);
	#endif
	head->wszFromNick = ConvertCodedStringToUnicode(ShortHeader.FromNick, CP, MIME_MAIL);
	#ifdef DEBUG_DECODE
	if (NULL != head->FromNick)
		mir_writeLogW(DecodeFile, L"<Converted from-nick>%s</Converted>\n", head->FromNick);
	#endif
	head->wszReturnPath = ConvertCodedStringToUnicode(ShortHeader.ReturnPath, CP, MIME_PLAIN);
	#ifdef DEBUG_DECODE
	if (NULL != head->ReturnPath)
		mir_writeLogW(DecodeFile, L"<Converted return-path>%s</Converted>\n", head->ReturnPath);
	#endif
	head->wszReturnPathNick = ConvertCodedStringToUnicode(ShortHeader.ReturnPathNick, CP, MIME_MAIL);
	#ifdef DEBUG_DECODE
	if (NULL != head->ReturnPathNick)
		mir_writeLogW(DecodeFile, L"<Converted return-path nick>%s</Converted>\n", head->ReturnPathNick);
	#endif
	head->wszSubject = ConvertCodedStringToUnicode(ShortHeader.Subject, CP, MIME_PLAIN);
	#ifdef DEBUG_DECODE
	if (NULL != head->Subject)
		mir_writeLogW(DecodeFile, L"<Converted subject>%s</Converted>\n", head->Subject);
	#endif
	head->wszDate = ConvertCodedStringToUnicode(ShortHeader.Date, CP, MIME_PLAIN);
	#ifdef DEBUG_DECODE
	if (NULL != head->Date)
		mir_writeLogW(DecodeFile, L"<Converted date>%s</Converted>\n", head->Date);
	#endif

	head->wszBody = ConvertCodedStringToUnicode(ShortHeader.Body, CP, MIME_PLAIN);
	#ifdef DEBUG_DECODE
	if (NULL != head->Body)
		mir_writeLogW(DecodeFile, L"<Converted Body>%s</Converted>\n", head->Body);
	#endif

	#ifdef DEBUG_DECODE
	mir_writeLogA(DecodeFile, "</Convert>\n");
	#endif

	DeleteShortHeaderContent(&ShortHeader);
}

#define TE_UNKNOWN
#define TE_QUOTEDPRINTABLE 1
#define TE_BASE64 2
struct APartDataType
{
	char *Src;// Input
	char *ContType;
	int CodePage;
	char *TransEnc;
	uint8_t TransEncType; // TE_something
	char *body;
	int bodyLen;
	wchar_t *wBody;
};

void ParseAPart(APartDataType *data)
{
	size_t len = mir_strlen(data->Src);
	try {
		char *finder = data->Src;
		char *prev1, *prev2, *prev3;

		while (finder <= (data->Src + len)) {
			while (ENDLINEWS(finder)) finder++;

			// at the start of line
			if (finder > data->Src) {
				if (*(finder - 2) == '\r' || *(finder - 2) == '\n')
					*(finder - 2) = 0;
				if (*(finder - 1) == '\r' || *(finder - 1) == '\n')
					*(finder - 1) = 0;
			}
			prev1 = finder;

			while (*finder != ':' && !EOS(finder) && !ENDLINE(finder)) finder++;
			if (ENDLINE(finder) || EOS(finder)) {
				// no ":" in the line? here the body begins;
				data->body = prev1;
				break;
			}
			prev2 = finder++;

			while (WS(finder) && !EOS(finder)) finder++;
			if (!EOS(finder))
				prev3 = finder;
			else
				break;

			do {
				if (ENDLINEWS(finder)) finder += 2;						// after endline information continues
				while (!ENDLINE(finder) && !EOS(finder)) finder++;
			} while (ENDLINEWS(finder));

			if (!_strnicmp(prev1, "Content-type", prev2 - prev1)) {
				data->ContType = prev3;
			}
			else if (!_strnicmp(prev1, "Content-Transfer-Encoding", prev2 - prev1)) {
				data->TransEnc = prev3;
			}

			if (EOS(finder))
				break;
			finder++;
			if (ENDLINE(finder)) {
				finder++;
				if (ENDLINE(finder)) {
					// end of headers. message body begins
					if (finder > data->Src) {
						if (*(finder - 2) == '\r' || *(finder - 2) == '\n')
							*(finder - 2) = 0;
						if (*(finder - 1) == '\r' || *(finder - 1) == '\n')
							*(finder - 1) = 0;
					}
					finder++;
					if (ENDLINE(finder))finder++;
					prev1 = finder;
					while (!EOS(finder + 1))finder++;
					if (ENDLINE(finder))finder--;
					prev2 = finder;
					if (prev2 > prev1) { // yes, we have body
						data->body = prev1;
					}
					break; // there is nothing else
				}
			}
		}
	}
	catch (...) {
		MessageBox(nullptr, TranslateT("Translate header error"), L"", 0);
	}
	if (data->body) data->bodyLen = (int)mir_strlen(data->body);
}

// from decode.cpp
int DecodeQuotedPrintable(char *Src, char *Dst, int DstLen, BOOL isQ);
int ConvertStringToUnicode(char *stream, unsigned int cp, wchar_t **out);

wchar_t *ParseMultipartBody(char *src, char *bond)
{
	char *srcback = _strdup(src);
	size_t sizebond = mir_strlen(bond);
	int numparts = 1;
	int i;
	char *courbond = srcback;
	wchar_t *dest;
	for (; (courbond = strstr(courbond, bond)); numparts++, courbond += sizebond);
	APartDataType *partData = new APartDataType[numparts];
	memset(partData, 0, sizeof(APartDataType) * numparts);
	partData[0].Src = courbond = srcback;
	for (i = 1; (courbond = strstr(courbond, bond)); i++, courbond += sizebond) {
		*(courbond - 2) = 0;
		partData[i].Src = courbond + sizebond;
		while (ENDLINE(partData[i].Src)) partData[i].Src++;
	}
	size_t resultSize = 0;
	for (i = 0; i < numparts; i++) {
		ParseAPart(&partData[i]);
		if (partData[i].body) {
			if (partData[i].TransEnc) {
				if (!_stricmp(partData[i].TransEnc, "base64")) partData[i].TransEncType = TE_BASE64;
				else if (!_stricmp(partData[i].TransEnc, "quoted-printable"))partData[i].TransEncType = TE_QUOTEDPRINTABLE;
			}
			if (partData[i].ContType) {
				char *CharSetStr;
				if (nullptr != (CharSetStr = ExtractFromContentType(partData[i].ContType, "charset="))) {
					partData[i].CodePage = GetCharsetFromString(CharSetStr, mir_strlen(CharSetStr));
					delete[] CharSetStr;
				}
			}
			if (partData[i].ContType && !_strnicmp(partData[i].ContType, "text", 4)) {
				ptrA localBody;
				switch (partData[i].TransEncType) {
				case TE_BASE64:
					localBody = (char*)mir_base64_decode(partData[i].body, 0);
					break;
				case TE_QUOTEDPRINTABLE:
					{
						int size = partData[i].bodyLen + 2;
						localBody = (char*)mir_alloc(size + 1);
						DecodeQuotedPrintable(partData[i].body, localBody, size, FALSE);
					}
					break;
				}
				ConvertStringToUnicode(localBody ? localBody : partData[i].body, partData[i].CodePage, &partData[i].wBody);
			}
			else if (partData[i].ContType && !_strnicmp(partData[i].ContType, "multipart/", 10)) {
				// Multipart in mulitipart recursive? should be SPAM. Ah well
				char *bondary = nullptr;
				if (nullptr != (bondary = ExtractFromContentType(partData[i].ContType, "boundary="))) {
					partData[i].wBody = ParseMultipartBody(partData[i].body, bondary);
					delete[] bondary;
				}
				else goto FailBackRaw; // multipart with no boundary? badly formatted messages.
			}
			else {
FailBackRaw:
				ConvertStringToUnicode(partData[i].body, partData[i].CodePage, &partData[i].wBody);
			}
			resultSize += mir_wstrlen(partData[i].wBody);
		}// if (partData[i].body)
		resultSize += 100 + 4 + 3; // cr+nl+100+ 3*bullet
	}
	dest = new wchar_t[resultSize + 1];
	size_t destpos = 0;
	for (i = 0; i < numparts; i++) {
		if (i) { // part before first boudary should not have headers
			char infoline[1024]; size_t linesize = 0;
			mir_snprintf(infoline, "%s %d", Translate("Part"), i);
			linesize = mir_strlen(infoline);
			if (partData[i].TransEnc) {
				mir_snprintf(infoline + linesize, _countof(infoline) - linesize, "; %s", partData[i].TransEnc);
				linesize = mir_strlen(infoline);
			}
			if (partData[i].ContType) {
				char *CharSetStr = strchr(partData[i].ContType, ';');
				if (CharSetStr) {
					CharSetStr[0] = 0;
					mir_snprintf(infoline + linesize, _countof(infoline) - linesize, "; %s", partData[i].ContType);
					linesize = mir_strlen(infoline);
					partData[i].ContType = CharSetStr + 1;
					if (nullptr != (CharSetStr = ExtractFromContentType(partData[i].ContType, "charset="))) {
						mir_snprintf(infoline + linesize, _countof(infoline) - linesize, "; %s", CharSetStr);
						linesize = mir_strlen(infoline);
						delete[] CharSetStr;
					}
					if (nullptr != (CharSetStr = ExtractFromContentType(partData[i].ContType, "name="))) {
						mir_snprintf(infoline + linesize, _countof(infoline) - linesize, "; \"%s\"", CharSetStr);
						linesize = mir_strlen(infoline);
						delete[] CharSetStr;
					}
				}
				else {
					mir_snprintf(infoline + linesize, _countof(infoline) - linesize, "; %s", partData[i].ContType);
					linesize = mir_strlen(infoline);
				}
			}
			mir_snprintf(infoline + linesize, _countof(infoline) - linesize, ".\r\n");
			{
				wchar_t *temp = nullptr;
				dest[destpos] = dest[destpos + 1] = dest[destpos + 2] = 0x2022; // bullet;
				destpos += 3;
				ConvertStringToUnicode(infoline, CP_ACP, &temp);
				size_t wsize = mir_wstrlen(temp);
				mir_wstrcpy(&dest[destpos], temp);
				destpos += wsize;
				delete[] temp;
			}
		} // if (i)

		if (partData[i].wBody) {
			size_t wsize = mir_wstrlen(partData[i].wBody);
			mir_wstrcpy(&dest[destpos], partData[i].wBody);
			destpos += wsize;
			delete[] partData[i].wBody;
		}
	}

	free(srcback);
	delete[] partData;
	dest[resultSize] = 0;// just in case
	return dest;
}
