// Copyright © 2008 sss, chaos.persei
// 
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#ifndef COMMONHEADERS_H
#define COMMONHEADERS_H

#include <windows.h>

#include <newpluginapi.h>
#include <m_database.h>
#include <m_netlib.h>
#include <m_utils.h>
#include <m_options.h>
#include <m_langpack.h>
#include <m_gui.h>

#include <m_music.h>

#include "resource.h"
#include "version.h"

#define MODULENAME "Watrack_MPD"

struct CMPlugin : public PLUGIN<CMPlugin>
{
	CMPlugin();

	int Load() override;
	int Unload() override;
};

extern HNETLIBUSER ghNetlibUser;
extern BOOL bWatrackService;
extern wchar_t *gbHost, *gbPassword;
extern uint16_t gbPort;

extern int WaMpdOptInit(WPARAM, LPARAM);
extern void RegisterPlayer();

#endif
