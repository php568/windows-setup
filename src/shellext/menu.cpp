
#include "dll.h"
#include "menu.h"
#include "register.h"
#include <stdio.h>
#include <new>
#include <Objbase.h>
#include <strsafe.h>
#include <Shlwapi.h>
#include <uxtheme.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uxtheme.lib")


extern long g_DllRef;
extern CRITICAL_SECTION g_ShellCs;
extern CSXGLOBALS globals;

ComposerShellMenu::ComposerShellMenu(void) :
    m_RefCount(1),
    m_Folder(NULL),
    m_Data(NULL)
{

    InterlockedIncrement(&g_DllRef);

    EnterCriticalSection(&g_ShellCs);
    
    m_Globals.module = globals.module; 
    RunasInit();
    m_Globals.shield = globals.shield;
    m_Globals.shieldSet = globals.shieldSet;
    
    LeaveCriticalSection(&g_ShellCs);
    RunasGet();
}

ComposerShellMenu::~ComposerShellMenu(void)
{
    ClearStorage();
    InterlockedDecrement(&g_DllRef);
}


// Query to the interface the component supported.
IFACEMETHODIMP ComposerShellMenu::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] = 
    {
        QITABENT(ComposerShellMenu, IContextMenu),
        QITABENT(ComposerShellMenu, IShellExtInit), 
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}


// Increase the reference count for an interface on an object.
IFACEMETHODIMP_(ULONG) ComposerShellMenu::AddRef()
{
    return InterlockedIncrement(&m_RefCount);
}

// Decrease the reference count for an interface on an object.
IFACEMETHODIMP_(ULONG) ComposerShellMenu::Release()
{
    ULONG cRef = InterlockedDecrement(&m_RefCount);
    if (0 == cRef)
    {
        delete this;
    }

    return cRef;
}


// Initialize the context menu handler.
IFACEMETHODIMP ComposerShellMenu::Initialize(
    LPCITEMIDLIST pidlFolder, LPDATAOBJECT pDataObj, HKEY hKeyProgID)
{

    // We store the input for any subsequent QueryContextMenu call
    ClearStorage();
    HRESULT hr = E_INVALIDARG;

    if (pidlFolder)
    {
        m_Folder = ILClone(pidlFolder);
        hr = m_Folder ? S_OK : E_OUTOFMEMORY;
    }
    else if (pDataObj)
    {
        m_Data = pDataObj; 
        m_Data->AddRef();
        hr = S_OK;
    }

    return hr;
}


IFACEMETHODIMP ComposerShellMenu::QueryContextMenu(
    HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    
    // Do nothing if uFlags include CMF_DEFAULTONLY.
    if (CMF_DEFAULTONLY & uFlags)
        return S_OK;
    
    // See if we have already written the menu.
    if (MenuExists(hMenu))
        return S_OK;

    // Get the target directory and status
    HRESULT hr = GetTargetData();
    if (SUCCEEDED(hr))
    {
        DWORD result = 0;
        if (!MenuBuild(hMenu, indexMenu, idCmdFirst, &result))
        {
            hr = HRESULT_FROM_WIN32(result);
        }
        else
        {
            hr = MAKE_HRESULT(SEVERITY_SUCCESS, 0, USHORT(result));
        }
    }
        
    return hr;
}


IFACEMETHODIMP ComposerShellMenu::InvokeCommand(LPCMINVOKECOMMANDINFO pici)
{

    HRESULT hr = E_FAIL;
    CSXINFO info;
    
    if (GetInfoFromCmdId(LOWORD(pici->lpVerb), &info))
    {
        std::wstring params;
        std::wstring echo;

        if (CMD_ADMIN == info.id)
        {
            return RunasToggle();   
        }
        else if (CMD_SHELL == info.id)
        {
            echo = L"ECHO. && ECHO Basic usage: composer ^<command^> && ECHO.";
            echo.append(L"For more information just type \"composer\".");
            params = L"/k " + echo;
        }
        else
        {
            std::wstring cmd(L"composer ");
            cmd.append(info.cmd);
            
            echo = L"ECHO. && ECHO Command: " + cmd + L" && ECHO.";
            params = L"/k " + echo + L" && " + cmd;
        }

        BOOLEAN open = !RunasUse(info.id);
        
        if (open)
        {
            ShellExecute(pici->hwnd, L"open", L"cmd.exe", params.c_str(), m_TargetDir.c_str(), SW_SHOWNORMAL);
        }
        else
        {
            params.insert(3, L"cd " + m_TargetDir + L" && ");
            ShellExecute(pici->hwnd, L"runas", L"cmd.exe", params.c_str(), NULL, SW_SHOWNORMAL);
        }

        hr = S_OK;
    }
    
    return hr;
}


IFACEMETHODIMP ComposerShellMenu::GetCommandString(UINT_PTR idCommand, 
    UINT uFlags, UINT *pwReserved, LPSTR pszName, UINT cchMax)
{
    CSXINFO info;
    if (!GetInfoFromCmdId(idCommand, &info))
    {
        return uFlags == GCS_VALIDATEW ? S_FALSE : E_INVALIDARG; 
    }

    HRESULT hr = E_INVALIDARG;
    switch (uFlags)
    {
    case GCS_HELPTEXTW:
        if (GetInfoFromCmdId(idCommand, &info))
        {
            hr = StringCchCopy(reinterpret_cast<PWSTR>(pszName), cchMax, info.help);
        }
        break;

    case GCS_VERBW:
        break;;

    default:
        hr = S_OK;
    }

    return hr;
}


void ComposerShellMenu::ClearStorage()
{
    CoTaskMemFree(m_Folder);
    m_Folder = NULL;
    
    if (m_Data)
    { 
        m_Data->Release();
        m_Data = NULL;
    }
}


BOOLEAN ComposerShellMenu::DirectoryExists(const std::wstring& path)
{
    DWORD attrib = GetFileAttributes(path.c_str());

    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
}


BOOLEAN ComposerShellMenu::FileExists(const std::wstring& filepath)
{
    DWORD attrib = GetFileAttributes(filepath.c_str());

    return (attrib != INVALID_FILE_ATTRIBUTES) && ((attrib & FILE_ATTRIBUTE_DIRECTORY) == 0);
}


BOOLEAN ComposerShellMenu::GetInfoFromCmdId(UINT_PTR cmdId, LPCSXINFO csxInfo)
{
    int count = int(m_IdList.size());

    for (int i = 0; i < count; ++i)
    {
        if (cmdId == m_IdList[i].id)
        {
            return GetCmdInfo(m_IdList[i].info, csxInfo);
        }

    }

    return false;
}


BOOLEAN ComposerShellMenu::GetCmdInfo(UINT csxId, LPCSXINFO csxInfo)
{

    for (int i = 0; i < ARRAYSIZE(COMPOSER_INFO); ++i)
    {
        if (csxId == COMPOSER_INFO[i].id)
        {
            *csxInfo = COMPOSER_INFO[i]; 
            return true;
        }
    }

    return false;
}

HRESULT ComposerShellMenu::GetDataFromShellItem(IShellItem* item)
{
    LPWSTR filepath;
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filepath);
            
    if (SUCCEEDED(hr))
    {
        m_TargetDir = filepath;
        CoTaskMemFree(filepath);
        
        SetStatus();
    }

    return hr;
}


// Populates item. It is the responsibility of the caller to call item->Release()
// if the return value indicates success
HRESULT ComposerShellMenu::GetShellItem(IShellItem ** item, PBOOLEAN isFile)
{
    HRESULT hr = E_FAIL;
    *item = NULL;
        
    if (m_Folder)
    {
        hr = SHCreateItemFromIDList(m_Folder, IID_PPV_ARGS(item));
    }
    else if (m_Data)
    {
        IShellItemArray *itemArray = NULL;
        hr = SHCreateShellItemArrayFromDataObject(m_Data, IID_PPV_ARGS(&itemArray));
        
        if (SUCCEEDED(hr))
        {
            DWORD count = 0;
            hr = itemArray->GetCount(&count);           
            
            if (SUCCEEDED(hr))
            {
                if (1 == count)
                {
                    hr = itemArray->GetItemAt(0, item);
                }
                else
                {
                    hr = E_FAIL;
                }
            }
            
            itemArray->Release();
            itemArray = NULL;
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = IsShellItemValid(*item, false, isFile);

        if (FAILED(hr))
        {
            (*item)->Release();
            *item = NULL;
        }
    }

    return hr;
}


// Populates item. It is the responsibility of the caller to call item->Release()
// if the return value indicates success
HRESULT ComposerShellMenu::GetShellItemParent(IShellItem* child, IShellItem ** item)
{
    BOOLEAN isFile = false;
    HRESULT hr = child->GetParent(item);

    if (SUCCEEDED(hr))
    {
        hr = IsShellItemValid(*item, true, &isFile);

        if (FAILED(hr))
        {
            (*item)->Release();
            *item = NULL;
        }
    }

    return hr;
}


HRESULT ComposerShellMenu::GetTargetData()
{
    IShellItem* item = NULL;
    BOOLEAN isFile = false;
    HRESULT hr = GetShellItem(&item, &isFile);
    
    if (SUCCEEDED(hr))
    {
        if (isFile)
        {
            IShellItem* parent = NULL;
            hr = GetShellItemParent(item, &parent);

            if (SUCCEEDED(hr))
            {
                hr = this->GetDataFromShellItem(parent);
                parent->Release();
            }
        }
        else
        {
            hr = this->GetDataFromShellItem(item);
        }
        
        item->Release();
    }

    return hr;
}


HBITMAP ComposerShellMenu::IconToBitmap(HICON hIcon)
{
    HBITMAP hBmp = NULL;
    HDC hdcDest = CreateCompatibleDC(NULL);

    if (hIcon && hdcDest)
    {
        long cx = GetSystemMetrics(SM_CXSMICON);
        long cy = GetSystemMetrics(SM_CYSMICON);
        RGBQUAD *bits = NULL;

        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biCompression = BI_RGB;
        bmi.bmiHeader.biWidth = cx;
        bmi.bmiHeader.biHeight = cy;
        bmi.bmiHeader.biBitCount = 32;

        hBmp = CreateDIBSection(hdcDest, &bmi, DIB_RGB_COLORS, (VOID **)bits, NULL, 0);
        if (hBmp)
        { 
            SelectObject(hdcDest, hBmp);
            RECT rct;
            SetRect(&rct, 0, 0, cx, cy);

            BLENDFUNCTION bfAlpha = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            BP_PAINTPARAMS paintParams = {0};
            paintParams.cbSize = sizeof(paintParams);
            paintParams.dwFlags = BPPF_ERASE;
            paintParams.pBlendFunction = &bfAlpha;

            HDC hdcBuffer;
            HPAINTBUFFER hPaintBuffer = BeginBufferedPaint(hdcDest, &rct, BPBF_DIB, &paintParams, &hdcBuffer);
            if (hPaintBuffer)
            {
                DrawIconEx(hdcBuffer, 0, 0, hIcon, cx, cy, 0, NULL, DI_NORMAL);
                // Write the buffer contents to hdcDest
                EndBufferedPaint(hPaintBuffer, TRUE);
            }
        }

        DeleteDC(hdcDest);
    }

    return hBmp;
}


HRESULT ComposerShellMenu::IsShellItemValid(IShellItem* item, BOOLEAN requireFolder, PBOOLEAN isFile)
{
    DWORD mask = SFGAO_FILESYSTEM | SFGAO_STREAM | SFGAO_FOLDER;
    DWORD attribs = 0;
    HRESULT hr = item->GetAttributes(mask, &attribs);

    // See if we have a com error
    if (S_OK != hr && S_FALSE != hr)
        return hr;

    // Ensure we have SFGAO_FILESYSTEM 
    if (!(attribs & SFGAO_FILESYSTEM))
        return E_FAIL;

    // A compressed file can have both SFGAO_STREAM and SFGAO_FOLDER
    BOOLEAN folder = (attribs & SFGAO_FOLDER) && !(attribs & SFGAO_STREAM);
    *isFile = !folder;
    
    return requireFolder && *isFile ? E_FAIL : S_OK;
}


BOOLEAN ComposerShellMenu::MenuAdd(UINT infoId, HMENU hMenu, HMENU hSub, UINT idCmdFirst, PUINT cmdId, PUINT position, PDWORD error)
{
    CSXINFO info;
    
    if (!GetCmdInfo(infoId, &info))
    {
        *error = 87;
        return false;
    }

    MENUITEMINFO mii = { sizeof(mii) };
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_STRING | MIIM_DATA | MIIM_ID;
    mii.wID = idCmdFirst + *cmdId;
    mii.dwItemData = (ULONG_PTR)m_Globals.module;
    mii.dwTypeData = info.title;
    
    if (hSub)
    {
        mii.fMask |= MIIM_SUBMENU;
        mii.hSubMenu = hSub;
    }

    if (CMD_ADMIN == infoId && CSX_ADMIN == m_Globals.runas)
    {
        mii.fMask |= MIIM_STATE;
        mii.fState = MFS_CHECKED; 
    }
    else if (RunasUse(infoId))
    {
        mii.fMask |= MIIM_BITMAP;
        mii.hbmpItem = RunasGetBitmap();
    }
    
    if (!InsertMenuItem(hMenu, *position, TRUE, &mii))
    {
        *error = GetLastError();
        return false;
    }

    CSXIDREC rec = {*cmdId, infoId};
    m_IdList.push_back(rec);

    *cmdId += 1;
    *position += 1;

    return true;
}


// On success returns TRUE with incremented cmdId in result
// On failure returns FALSE with Win32 error code in result
BOOLEAN ComposerShellMenu::MenuBuild(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, PDWORD result)
{
    UINT cmdId = 0;
    UINT position = indexMenu + 1;

    // See if the target is valid
    if (m_Status.invalid)
    {
        *result = 0;
        return true;
    }

    InsertMenu(hMenu, position++, MF_SEPARATOR|MF_BYPOSITION, 0, NULL); cmdId++;

    // init if not composer folder
    if (FALSE == m_Status.composer)
    {
        if (!MenuAdd(CMD_INIT, hMenu, 0, idCmdFirst, &cmdId, &position, result))
            return false;
        
    }
    else
    {
        // install
        if (!MenuAdd(CMD_INSTALL, hMenu, 0, idCmdFirst, &cmdId, &position, result))
            return false;

        // update if installed
        if (m_Status.installed)
        {
            if (!MenuAdd(CMD_UPDATE, hMenu, 0, idCmdFirst, &cmdId, &position, result))
                return false;
        }
        
    }

    UINT subIndex = 0;
    HMENU submenu = CreatePopupMenu();

    if (TRUE == m_Status.composer)
    {

        // Submenu install prefer-dist and install prefer-source
        if (!MenuAdd(CMD_INSTALL_DST, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
            return false;

        if (!MenuAdd(CMD_INSTALL_SRC, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
            return false;

        InsertMenu(submenu, subIndex++, MF_SEPARATOR|MF_BYPOSITION, 0, NULL); cmdId++;

        // Submenu update prefer-dist and update prefer-source if installed
        if (m_Status.installed)
        {
            if (!MenuAdd(CMD_UPDATE_DST, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
                return false;

            if (!MenuAdd(CMD_UPDATE_SRC, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
                return false;

            InsertMenu(submenu, subIndex++, MF_SEPARATOR|MF_BYPOSITION, 0, NULL); cmdId++;
        }

        // Submenu dump-autload and dump-autoload --optimize
        if (!MenuAdd(CMD_DUMP_AUTOLOAD, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
            return false;

        if (!MenuAdd(CMD_DUMP_AUTOLOAD_OPT, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
            return false;

        InsertMenu(submenu, subIndex++, MF_SEPARATOR|MF_BYPOSITION, 0, NULL); cmdId++;
    }

    // Submenu fixed items
    if (!MenuAdd(CMD_SELF_UPDATE, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
        return false;

    if (!MenuAdd(CMD_HELP, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
        return false;

    if (!MenuAdd(CMD_ADMIN, submenu, 0, idCmdFirst, &cmdId, &subIndex, result))
        return false;

    // Add the submenu
    if (!MenuAdd(CMD_OPTIONS, hMenu, submenu, idCmdFirst, &cmdId, &position, result))
        return false;

    // Shell
    if (!MenuAdd(CMD_SHELL, hMenu, 0, idCmdFirst, &cmdId, &position, result))
        return false;

    InsertMenu(hMenu, position++, MF_SEPARATOR|MF_BYPOSITION, 0, NULL); cmdId++;

    *result = cmdId;
    return true;
}


BOOLEAN ComposerShellMenu::MenuExists(HMENU menu)
{

    MENUITEMINFO mii = { sizeof(mii) };
    mii.cbSize = sizeof(MENUITEMINFO);
    mii.fMask = MIIM_DATA;
    mii.dwTypeData = NULL;

    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        GetMenuItemInfo(menu, i, TRUE, &mii);
        if (mii.dwItemData == (ULONG_PTR)m_Globals.module)
        {
            return true;
        }
    }

    return false;
}


// not currently used
std::wstring ComposerShellMenu::ReadComposerJson()
{
    std::wstring buffer;
    std::wstring filename(m_TargetDir);
    filename.append(L"\\composer.json");
    FILE* f;
    _wfopen_s(&f, filename.c_str(), L"rtS, ccs=UTF-8");

    if (f == NULL)
    {
        return buffer;
    }

    struct _stat fileinfo;
    _wstat(filename.c_str(), &fileinfo);
    size_t filesize = fileinfo.st_size;

    // Read the first kb
    filesize = filesize > 1024 ? 1024 : filesize;

    if (filesize > 0)
    {
        buffer.resize(filesize);
        size_t wchars_read = fread(&(buffer.front()), sizeof(wchar_t), filesize, f);
        buffer.resize(wchars_read);
        buffer.shrink_to_fit();
    }

    fclose(f);

    return buffer;
}


void ComposerShellMenu::RunasGet()
{
    if (m_Globals.runasSet)
        return;

    m_Globals.runas = CSX_USER; 
    
    HKEY hKey = NULL;
    wchar_t subKey[MAX_PATH];

    HRESULT hr = StringCchPrintf(subKey, MAX_PATH, L"Software\\%s", COMPOSER_NAME);
    if (FAILED(hr)) return;

    if (ERROR_SUCCESS != RegOpenKeyEx(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey))
        return;

    DWORD value = 0;
    DWORD cbData = sizeof(value);
    if (ERROR_SUCCESS == RegQueryValueEx(hKey, COMPOSER_RUNAS, NULL, NULL, reinterpret_cast<BYTE *>(&value), &cbData))
    {
        EnterCriticalSection(&g_ShellCs);
        globals.runas = value;
        LeaveCriticalSection(&g_ShellCs);
        m_Globals.runas = value;
    }
}


HBITMAP ComposerShellMenu::RunasGetBitmap()
{

    if (m_Globals.shieldSet)
        return m_Globals.shield;

    EnterCriticalSection(&g_ShellCs);
        
    if (globals.shieldSet)
    {
        m_Globals.shieldSet = true;
        m_Globals.shield = globals.shield;
    }
    else
    {
        // we are going to set it below
        globals.shieldSet = true;
    }
    
    LeaveCriticalSection(&g_ShellCs);

    // return is we were set from globals
    if (m_Globals.shieldSet)
        return m_Globals.shield;

    m_Globals.shieldSet = true;
    SHSTOCKICONINFO sii = {0};
    sii.cbSize = sizeof(sii);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &sii)))
    {
        m_Globals.shield = IconToBitmap(sii.hIcon);

        if (m_Globals.shield)
        {
            EnterCriticalSection(&g_ShellCs);
            globals.shield = m_Globals.shield;
            LeaveCriticalSection(&g_ShellCs);
        }
    
        DestroyIcon(sii.hIcon);
    }

    return m_Globals.shield;
}


void ComposerShellMenu::RunasInit()
{
    // We are called in a critical section
    m_Globals.runas = globals.runas;
    m_Globals.runasSet = globals.runasSet;
    
    if (!globals.runasSet)
    {
        globals.runasSet = true;
    }
}


HRESULT ComposerShellMenu::RunasSet(DWORD value)
{
    HRESULT hr = E_FAIL;
        
    if (CSX_USER != value && CSX_ADMIN != value)
        return hr;

    HKEY hKey = NULL;
    wchar_t subKey[MAX_PATH];

    hr = StringCchPrintf(subKey, MAX_PATH, L"Software\\%s", COMPOSER_NAME);
    if (FAILED(hr)) return hr;

    hr = HRESULT_FROM_WIN32(RegCreateKeyEx(HKEY_CURRENT_USER, subKey, 0, 
        NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL));
    if (FAILED(hr)) return hr;

    DWORD cbData = sizeof(value);
    hr = HRESULT_FROM_WIN32(RegSetValueEx(hKey, COMPOSER_RUNAS, 0, REG_DWORD,
        reinterpret_cast<const BYTE *>(&value), cbData));

    if (SUCCEEDED(hr))
    {
        EnterCriticalSection(&g_ShellCs);
        globals.runas = value;
        LeaveCriticalSection(&g_ShellCs);
        m_Globals.runas = value;
    }

    return hr;
}


HRESULT ComposerShellMenu::RunasToggle()
{
    DWORD value = CSX_USER == m_Globals.runas ? CSX_ADMIN : CSX_USER;

    return RunasSet(value);
}


BOOLEAN ComposerShellMenu::RunasUse(int cmdId)
{
    return (CSX_ADMIN == m_Globals.runas && (
        //CMD_SHELL == cmdId ||
        CMD_INSTALL == cmdId ||
        CMD_INSTALL_DST == cmdId ||
        CMD_INSTALL_SRC == cmdId ||
        CMD_UPDATE == cmdId ||
        CMD_UPDATE_DST == cmdId ||
        CMD_UPDATE_SRC == cmdId
        ));
}


void ComposerShellMenu::SetStatus()
{
    m_Status.invalid = false;

    // See if we are in vendor directory
    std::wstring path(m_TargetDir + L"\\");
    size_t pos = path.rfind(L"\\vendor\\");
    if (std::wstring::npos != pos)
    {
        path.resize(pos);
        m_Status.invalid = DirectoryExists(path + L"\\vendor\\composer");
    }
    
    if (m_Status.invalid)
        return;
        
    m_Status.composer = FileExists(m_TargetDir + L"\\composer.json");

    if (m_Status.composer)
    {
        // We have a composer.json. Check if we have installed it
        m_Status.installed = FileExists(m_TargetDir + L"\\vendor\\composer\\installed.json");
        
        /*
        //std::wstring composerJson = ReadComposerJson();
        m_Status.project = std::wstring::npos == composerJson.find(L"\"name\":");
        m_Status.package = !m_Status.project;
        */
    }
}
