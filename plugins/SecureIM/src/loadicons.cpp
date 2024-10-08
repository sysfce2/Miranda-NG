#include "commonheaders.h"

struct
{
	int  key; // Resource ID
	uint8_t tbl; // Table NUM
	uint8_t idx; // Table IDX
	char *section;
	char *name;
	char *text;
}
static icons[] =
{
	// Contact List
	{ IDI_CL_DIS, TBL_IEC, IEC_CL_DIS, MODULENAME "/" LPGEN("Contact List"), "sim_cl_dis", LPGEN("Connection Disabled") },
	{ IDI_CL_EST, TBL_IEC, IEC_CL_EST, MODULENAME "/" LPGEN("Contact List"), "sim_cl_est", LPGEN("Connection Established") },

	// Contact Menu
	{ IDI_CM_DIS, TBL_ICO, ICO_CM_DIS, MODULENAME "/" LPGEN("Contact Menu"), "sim_cm_dis", LPGEN("Disable Secure Connection") },
	{ IDI_CM_EST, TBL_ICO, ICO_CM_EST, MODULENAME "/" LPGEN("Contact Menu"), "sim_cm_est", LPGEN("Establish Secure Connection") },

	// Message Window
	{ IDI_MW_DIS, TBL_ICO, ICO_MW_DIS, MODULENAME "/" LPGEN("Message Window"), "sim_mw_dis", LPGEN("Connection Disabled") },
	{ IDI_MW_EST, TBL_ICO, ICO_MW_EST, MODULENAME "/" LPGEN("Message Window"), "sim_mw_est", LPGEN("Connection Established") },

	// popup's
	{ IDI_PU_DIS, TBL_POP, POP_PU_DIS, MODULENAME "/" LPGEN("Popups"), "sim_pu_dis", LPGEN("Secure Connection Disabled") },
	{ IDI_PU_EST, TBL_POP, POP_PU_EST, MODULENAME "/" LPGEN("Popups"), "sim_pu_est", LPGEN("Secure Connection Established") },
	{ IDI_PU_PRC, TBL_POP, POP_PU_PRC, MODULENAME "/" LPGEN("Popups"), "sim_pu_prc", LPGEN("Secure Connection In Process") },
	{ IDI_PU_MSG, TBL_POP, POP_PU_MSR, MODULENAME "/" LPGEN("Popups"), "sim_pu_msr", LPGEN("Recv Secured Message") },
	{ IDI_PU_MSG, TBL_POP, POP_PU_MSS, MODULENAME "/" LPGEN("Popups"), "sim_pu_mss", LPGEN("Sent Secured Message") },

	// statuses
	{ IDI_ST_DIS, TBL_ICO, ICO_ST_DIS, MODULENAME "/" LPGEN("Menu State"), "sim_st_dis", LPGEN("Disabled") },
	{ IDI_ST_ENA, TBL_ICO, ICO_ST_ENA, MODULENAME "/" LPGEN("Menu State"), "sim_st_ena", LPGEN("Enabled") },
	{ IDI_ST_TRY, TBL_ICO, ICO_ST_TRY, MODULENAME "/" LPGEN("Menu State"), "sim_st_try", LPGEN("Always Try") },

	// overlay
	{ IDI_OV_NAT, TBL_ICO, ICO_OV_NAT, MODULENAME "/" LPGEN("Overlays"), "sim_ov_nat", LPGEN("Native mode") },
	{ IDI_OV_PGP, TBL_ICO, ICO_OV_PGP, MODULENAME "/" LPGEN("Overlays"), "sim_ov_pgp", LPGEN("PGP mode") },
	{ IDI_OV_GPG, TBL_ICO, ICO_OV_GPG, MODULENAME "/" LPGEN("Overlays"), "sim_ov_gpg", LPGEN("GPG mode") },
	{ IDI_OV_RSA, TBL_ICO, ICO_OV_RSA, MODULENAME "/" LPGEN("Overlays"), "sim_ov_rsa", LPGEN("RSA/AES mode") },
};

HINSTANCE LoadIconsPack(const char *szIconsPack)
{
	HINSTANCE hNewIconInst = LoadLibrary(szIconsPack);
	if (hNewIconInst != nullptr) {
		for (int i = ID_FIRSTICON; i <= ID_LASTICON; i++)
			if (LoadIcon(hNewIconInst, MAKEINTRESOURCE(i)) == nullptr) {
			FreeLibrary(hNewIconInst);
			hNewIconInst = nullptr;
			break;
			}
	}
	return hNewIconInst;
}

int ReloadIcons(WPARAM wParam, LPARAM lParam)
{
	for (int i = 0; i < _countof(icons); i++) {
		HICON hIcon = IcoLib_GetIcon(icons[i].name);
		if (icons[i].tbl == TBL_IEC)
			g_hIEC[icons[i].idx] = hIcon;
		else if (icons[i].tbl == TBL_ICO)
			g_hICO[icons[i].idx] = hIcon;
		else if (icons[i].tbl == TBL_POP)
			g_hPOP[icons[i].idx] = hIcon;
	}

	return 0;
}

void InitIcons(void)
{
	HINSTANCE hNewIconInst = nullptr;

	if (g_hFolders) {
		char pathname[MAX_PATH];
		FoldersGetCustomPath(g_hFolders, pathname, MAX_PATH, "icons\\");
		if (hNewIconInst == nullptr)
			hNewIconInst = LoadIconsPack(pathname);
	}

	if (hNewIconInst == nullptr)
		hNewIconInst = LoadIconsPack("icons\\secureim_icons.dll");

	if (hNewIconInst == nullptr)
		hNewIconInst = LoadIconsPack("plugins\\secureim_icons.dll");

	if (hNewIconInst == nullptr)
		g_hIconInst = g_plugin.getInst();
	else
		g_hIconInst = hNewIconInst;

	char tszPath[MAX_PATH];
	GetModuleFileName(g_hIconInst, tszPath, _countof(tszPath));

	SKINICONDESC sid = {};
	sid.section.a = "SecureIM";
	sid.defaultFile.a = tszPath;

	for (int i = 0; i < _countof(icons); i++) {
		sid.section.a = icons[i].section;
		sid.pszName = icons[i].name;
		sid.description.a = icons[i].text;
		sid.iDefaultIndex = -icons[i].key;
		HANDLE hIcolib = g_plugin.addIcon(&sid);

		HICON hIcon = IcoLib_GetIconByHandle(hIcolib);
		if (icons[i].tbl == TBL_IEC)
			g_hIEC[icons[i].idx] = hIcon;
		else if (icons[i].tbl == TBL_ICO)
			g_hICO[icons[i].idx] = hIcon;
		else if (icons[i].tbl == TBL_POP)
			g_hPOP[icons[i].idx] = hIcon;
	}

	HookEvent(ME_SKIN_ICONSCHANGED, ReloadIcons);

	g_hCLIcon = ExtraIcon_RegisterCallback(MODULENAME, LPGEN("SecureIM status"), "sim_cm_est", onExtraImageListRebuilding, onExtraImageApplying);
}
