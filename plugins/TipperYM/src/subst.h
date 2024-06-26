/*
Copyright (C) 2006-2007 Scott Ellis
Copyright (C) 2007-2011 Jan Holub

This is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this file; see the file license.txt.  If
not, write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  
*/

#ifndef _SUBST_INC
#define _SUBST_INC

bool GetLabelText(MCONTACT hContact, const DISPLAYITEM &di, wchar_t *buff, size_t iBufflen);
bool GetValueText(MCONTACT hContact, const DISPLAYITEM &di, wchar_t *buff, size_t iBufflen);

bool CheckContactType(MCONTACT hContact, const DISPLAYITEM &di);

void StripBBCodesInPlace(wchar_t *text);

// can be used with hContact == 0 to get uid for a given proto
bool UidName(char *szProto, wchar_t *buff, int bufflen); 
bool Uid(MCONTACT hContact, char *szProto, wchar_t *buff, int bufflen);

// get info for status and tray tooltip
bool DBGetContactSettingAsString(MCONTACT hContact, const char *szModuleName, const char *szSettingName, wchar_t *buff, int bufflen); 
bool CanRetrieveStatusMsg(MCONTACT hContact, char *szProto);
wchar_t *GetProtoStatusMessage(char *szProto, uint16_t status);
wchar_t *GetProtoExtraStatusTitle(char *szProto);
wchar_t *GetProtoExtraStatusMessage(char *szProto); 
wchar_t *GetListeningTo(char *szProto);
wchar_t *GetJabberAdvStatusText(char *szProto, const char *szSlot, const char *szValue); 
HICON GetJabberActivityIcon(MCONTACT hContact, char *szProto); 

#endif
