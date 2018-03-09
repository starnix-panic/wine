/*
 *	ICreateDevEnum implementation for DEVENUM.dll
 *
 * Copyright (C) 2002 Robert Shearman
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * NOTES ON THIS FILE:
 * - Implements ICreateDevEnum interface which creates an IEnumMoniker
 *   implementation
 * - Also creates the special registry keys created at run-time
 */

#define NONAMELESSSTRUCT
#define NONAMELESSUNION

#include "devenum_private.h"
#include "vfw.h"
#include "aviriff.h"

#include "wine/debug.h"
#include "wine/unicode.h"
#include "mmddk.h"

WINE_DEFAULT_DEBUG_CHANNEL(devenum);

extern HINSTANCE DEVENUM_hInstance;

static const WCHAR wszRegSeparator[] =   {'\\', 0 };
static const WCHAR wszFilterKeyName[] = {'F','i','l','t','e','r',0};
static const WCHAR wszMeritName[] = {'M','e','r','i','t',0};
static const WCHAR wszPins[] = {'P','i','n','s',0};
static const WCHAR wszAllowedMany[] = {'A','l','l','o','w','e','d','M','a','n','y',0};
static const WCHAR wszAllowedZero[] = {'A','l','l','o','w','e','d','Z','e','r','o',0};
static const WCHAR wszDirection[] = {'D','i','r','e','c','t','i','o','n',0};
static const WCHAR wszIsRendered[] = {'I','s','R','e','n','d','e','r','e','d',0};
static const WCHAR wszTypes[] = {'T','y','p','e','s',0};
static const WCHAR wszFriendlyName[] = {'F','r','i','e','n','d','l','y','N','a','m','e',0};
static const WCHAR wszWaveInID[] = {'W','a','v','e','I','n','I','D',0};
static const WCHAR wszWaveOutID[] = {'W','a','v','e','O','u','t','I','D',0};

static ULONG WINAPI DEVENUM_ICreateDevEnum_AddRef(ICreateDevEnum * iface);
static HRESULT register_codecs(void);

/**********************************************************************
 * DEVENUM_ICreateDevEnum_QueryInterface (also IUnknown)
 */
static HRESULT WINAPI DEVENUM_ICreateDevEnum_QueryInterface(ICreateDevEnum *iface, REFIID riid,
        void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
	IsEqualGUID(riid, &IID_ICreateDevEnum))
    {
        *ppv = iface;
	DEVENUM_ICreateDevEnum_AddRef(iface);
	return S_OK;
    }

    FIXME("- no interface IID: %s\n", debugstr_guid(riid));
    *ppv = NULL;
    return E_NOINTERFACE;
}

/**********************************************************************
 * DEVENUM_ICreateDevEnum_AddRef (also IUnknown)
 */
static ULONG WINAPI DEVENUM_ICreateDevEnum_AddRef(ICreateDevEnum * iface)
{
    TRACE("\n");

    DEVENUM_LockModule();

    return 2; /* non-heap based object */
}

/**********************************************************************
 * DEVENUM_ICreateDevEnum_Release (also IUnknown)
 */
static ULONG WINAPI DEVENUM_ICreateDevEnum_Release(ICreateDevEnum * iface)
{
    TRACE("\n");

    DEVENUM_UnlockModule();

    return 1; /* non-heap based object */
}

static HKEY open_special_category_key(const CLSID *clsid, BOOL create)
{
    WCHAR key_name[sizeof(wszActiveMovieKey)/sizeof(WCHAR) + CHARS_IN_GUID-1];
    HKEY ret;
    LONG res;

    strcpyW(key_name, wszActiveMovieKey);
    if (!StringFromGUID2(clsid, key_name + sizeof(wszActiveMovieKey)/sizeof(WCHAR)-1, CHARS_IN_GUID))
        return NULL;

    if(create)
        res = RegCreateKeyW(HKEY_CURRENT_USER, key_name, &ret);
    else
        res = RegOpenKeyExW(HKEY_CURRENT_USER, key_name, 0, KEY_READ, &ret);
    if (res != ERROR_SUCCESS) {
        WARN("Could not open %s\n", debugstr_w(key_name));
        return NULL;
    }

    return ret;
}

static void DEVENUM_ReadPinTypes(HKEY hkeyPinKey, REGFILTERPINS2 *rgPin)
{
    HKEY hkeyTypes = NULL;
    DWORD dwMajorTypes, i;
    REGPINTYPES *lpMediaType = NULL;
    DWORD dwMediaTypeSize = 0;

    if (RegOpenKeyExW(hkeyPinKey, wszTypes, 0, KEY_READ, &hkeyTypes) != ERROR_SUCCESS)
        return ;

    if (RegQueryInfoKeyW(hkeyTypes, NULL, NULL, NULL, &dwMajorTypes, NULL, NULL, NULL, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
    {
        RegCloseKey(hkeyTypes);
        return ;
    }

    for (i = 0; i < dwMajorTypes; i++)
    {
        HKEY hkeyMajorType = NULL;
        WCHAR wszMajorTypeName[64];
        DWORD cName = sizeof(wszMajorTypeName) / sizeof(WCHAR);
        DWORD dwMinorTypes, i1;

        if (RegEnumKeyExW(hkeyTypes, i, wszMajorTypeName, &cName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) continue;

        if (RegOpenKeyExW(hkeyTypes, wszMajorTypeName, 0, KEY_READ, &hkeyMajorType) != ERROR_SUCCESS) continue;

        if (RegQueryInfoKeyW(hkeyMajorType, NULL, NULL, NULL, &dwMinorTypes, NULL, NULL, NULL, NULL, NULL, NULL, NULL)
                    != ERROR_SUCCESS)
        {
            RegCloseKey(hkeyMajorType);
            continue;
        }

        for (i1 = 0; i1 < dwMinorTypes; i1++)
        {
            WCHAR wszMinorTypeName[64];
            CLSID *clsMajorType = NULL, *clsMinorType = NULL;
            HRESULT hr;

            cName = sizeof(wszMinorTypeName) / sizeof(WCHAR);
            if (RegEnumKeyExW(hkeyMajorType, i1, wszMinorTypeName, &cName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) continue;

            clsMinorType = CoTaskMemAlloc(sizeof(CLSID));
            if (!clsMinorType) continue;

            clsMajorType = CoTaskMemAlloc(sizeof(CLSID));
            if (!clsMajorType) goto error_cleanup_types;

            hr = CLSIDFromString(wszMinorTypeName, clsMinorType);
            if (FAILED(hr)) goto error_cleanup_types;

            hr = CLSIDFromString(wszMajorTypeName, clsMajorType);
            if (FAILED(hr)) goto error_cleanup_types;

            if (rgPin->nMediaTypes == dwMediaTypeSize)
            {
                DWORD dwNewSize = dwMediaTypeSize + (dwMediaTypeSize < 2 ? 1 : dwMediaTypeSize / 2);
                REGPINTYPES *lpNewMediaType;

                lpNewMediaType = CoTaskMemRealloc(lpMediaType, sizeof(REGPINTYPES) * dwNewSize);
                if (!lpNewMediaType) goto error_cleanup_types;

                lpMediaType = lpNewMediaType;
                dwMediaTypeSize = dwNewSize;
             }

            lpMediaType[rgPin->nMediaTypes].clsMajorType = clsMajorType;
            lpMediaType[rgPin->nMediaTypes].clsMinorType = clsMinorType;
            rgPin->nMediaTypes++;
            continue;

            error_cleanup_types:

            CoTaskMemFree(clsMajorType);
            CoTaskMemFree(clsMinorType);
        }

        RegCloseKey(hkeyMajorType);
    }

    RegCloseKey(hkeyTypes);

    if (lpMediaType && !rgPin->nMediaTypes)
    {
        CoTaskMemFree(lpMediaType);
        lpMediaType = NULL;
    }

    rgPin->lpMediaType = lpMediaType;
}

static void DEVENUM_ReadPins(HKEY hkeyFilterClass, REGFILTER2 *rgf2)
{
    HKEY hkeyPins = NULL;
    DWORD dwPinsSubkeys, i;
    REGFILTERPINS2 *rgPins = NULL;

    rgf2->dwVersion = 2;
    rgf2->u.s2.cPins2 = 0;
    rgf2->u.s2.rgPins2 = NULL;

    if (RegOpenKeyExW(hkeyFilterClass, wszPins, 0, KEY_READ, &hkeyPins) != ERROR_SUCCESS)
        return ;

    if (RegQueryInfoKeyW(hkeyPins, NULL, NULL, NULL, &dwPinsSubkeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL)
                != ERROR_SUCCESS)
    {
        RegCloseKey(hkeyPins);
        return ;
    }

    if (dwPinsSubkeys)
    {
        rgPins = CoTaskMemAlloc(sizeof(REGFILTERPINS2) * dwPinsSubkeys);
        if (!rgPins)
        {
            RegCloseKey(hkeyPins);
            return ;
        }
    }

    for (i = 0; i < dwPinsSubkeys; i++)
    {
        HKEY hkeyPinKey = NULL;
        WCHAR wszPinName[MAX_PATH];
        DWORD cName = sizeof(wszPinName) / sizeof(WCHAR);
        REGFILTERPINS2 *rgPin = &rgPins[rgf2->u.s2.cPins2];
        DWORD value, size, Type;
        LONG lRet;

        memset(rgPin, 0, sizeof(*rgPin));

        if (RegEnumKeyExW(hkeyPins, i, wszPinName, &cName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) continue;

        if (RegOpenKeyExW(hkeyPins, wszPinName, 0, KEY_READ, &hkeyPinKey) != ERROR_SUCCESS) continue;

        size = sizeof(DWORD);
        lRet = RegQueryValueExW(hkeyPinKey, wszAllowedMany, NULL, &Type, (BYTE *)&value, &size);
        if (lRet != ERROR_SUCCESS || Type != REG_DWORD)
            goto error_cleanup;
        if (value)
            rgPin->dwFlags |= REG_PINFLAG_B_MANY;

        size = sizeof(DWORD);
        lRet = RegQueryValueExW(hkeyPinKey, wszAllowedZero, NULL, &Type, (BYTE *)&value, &size);
        if (lRet != ERROR_SUCCESS || Type != REG_DWORD)
            goto error_cleanup;
        if (value)
            rgPin->dwFlags |= REG_PINFLAG_B_ZERO;

        size = sizeof(DWORD);
        lRet = RegQueryValueExW(hkeyPinKey, wszDirection, NULL, &Type, (BYTE *)&value, &size);
        if (lRet != ERROR_SUCCESS || Type != REG_DWORD)
            goto error_cleanup;
        if (value)
            rgPin->dwFlags |= REG_PINFLAG_B_OUTPUT;


        size = sizeof(DWORD);
        lRet = RegQueryValueExW(hkeyPinKey, wszIsRendered, NULL, &Type, (BYTE *)&value, &size);
        if (lRet != ERROR_SUCCESS || Type != REG_DWORD)
            goto error_cleanup;
        if (value)
            rgPin->dwFlags |= REG_PINFLAG_B_RENDERER;

        DEVENUM_ReadPinTypes(hkeyPinKey, rgPin);

        ++rgf2->u.s2.cPins2;
        continue;

        error_cleanup:

        RegCloseKey(hkeyPinKey);
    }

    RegCloseKey(hkeyPins);

    if (rgPins && !rgf2->u.s2.cPins2)
    {
        CoTaskMemFree(rgPins);
        rgPins = NULL;
    }

    rgf2->u.s2.rgPins2 = rgPins;
}

static HRESULT DEVENUM_RegisterLegacyAmFilters(void)
{
    HKEY hkeyFilter = NULL;
    DWORD dwFilterSubkeys, i;
    LONG lRet;
    IFilterMapper2 *pMapper = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_FilterMapper2, NULL, CLSCTX_INPROC,
                           &IID_IFilterMapper2, (void **) &pMapper);
    if (SUCCEEDED(hr))
    {
        lRet = RegOpenKeyExW(HKEY_CLASSES_ROOT, wszFilterKeyName, 0, KEY_READ, &hkeyFilter);
        hr = HRESULT_FROM_WIN32(lRet);
    }

    if (SUCCEEDED(hr))
    {
        lRet = RegQueryInfoKeyW(hkeyFilter, NULL, NULL, NULL, &dwFilterSubkeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        hr = HRESULT_FROM_WIN32(lRet);
    }

    if (SUCCEEDED(hr))
    {
        for (i = 0; i < dwFilterSubkeys; i++)
        {
            WCHAR wszFilterSubkeyName[64];
            DWORD cName = sizeof(wszFilterSubkeyName) / sizeof(WCHAR);
            WCHAR wszRegKey[MAX_PATH];
            HKEY hkeyInstance = NULL;

            if (RegEnumKeyExW(hkeyFilter, i, wszFilterSubkeyName, &cName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) continue;

            strcpyW(wszRegKey, wszActiveMovieKey);
            StringFromGUID2(&CLSID_LegacyAmFilterCategory, wszRegKey + strlenW(wszRegKey), CHARS_IN_GUID);

            strcatW(wszRegKey, wszRegSeparator);
            strcatW(wszRegKey, wszFilterSubkeyName);

            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, wszRegKey, 0, KEY_READ, &hkeyInstance) == ERROR_SUCCESS)
            {
                RegCloseKey(hkeyInstance);
            }
            else
            {
                /* Filter is registered the IFilterMapper(1)-way in HKCR\Filter. Needs to be added to
                 * legacy am filter category. */
                HKEY hkeyFilterClass = NULL;
                REGFILTER2 rgf2;
                CLSID clsidFilter;
                WCHAR wszFilterName[MAX_PATH];
                DWORD Type;
                DWORD cbData;
                HRESULT res;
                IMoniker *pMoniker = NULL;

                TRACE("Registering %s\n", debugstr_w(wszFilterSubkeyName));

                strcpyW(wszRegKey, clsid_keyname);
                strcatW(wszRegKey, wszRegSeparator);
                strcatW(wszRegKey, wszFilterSubkeyName);

                if (RegOpenKeyExW(HKEY_CLASSES_ROOT, wszRegKey, 0, KEY_READ, &hkeyFilterClass) != ERROR_SUCCESS)
                    continue;

                rgf2.dwMerit = 0;

                cbData = sizeof(wszFilterName);
                if (RegQueryValueExW(hkeyFilterClass, NULL, NULL, &Type, (LPBYTE)wszFilterName, &cbData) != ERROR_SUCCESS ||
                    Type != REG_SZ)
                    goto cleanup;

                cbData = sizeof(rgf2.dwMerit);
                if (RegQueryValueExW(hkeyFilterClass, wszMeritName, NULL, &Type, (LPBYTE)&rgf2.dwMerit, &cbData) != ERROR_SUCCESS ||
                    Type != REG_DWORD)
                    goto cleanup;

                DEVENUM_ReadPins(hkeyFilterClass, &rgf2);

                res = CLSIDFromString(wszFilterSubkeyName, &clsidFilter);
                if (FAILED(res)) goto cleanup;

                IFilterMapper2_RegisterFilter(pMapper, &clsidFilter, wszFilterName, &pMoniker, NULL, NULL, &rgf2);

                if (pMoniker)
                    IMoniker_Release(pMoniker);

                cleanup:

                if (hkeyFilterClass) RegCloseKey(hkeyFilterClass);

                if (rgf2.u.s2.rgPins2)
                {
                    UINT iPin;

                    for (iPin = 0; iPin < rgf2.u.s2.cPins2; iPin++)
                    {
                        if (rgf2.u.s2.rgPins2[iPin].lpMediaType)
                        {
                            UINT iType;

                            for (iType = 0; iType < rgf2.u.s2.rgPins2[iPin].nMediaTypes; iType++)
                            {
                                CoTaskMemFree((void*)rgf2.u.s2.rgPins2[iPin].lpMediaType[iType].clsMajorType);
                                CoTaskMemFree((void*)rgf2.u.s2.rgPins2[iPin].lpMediaType[iType].clsMinorType);
                            }

                            CoTaskMemFree((void*)rgf2.u.s2.rgPins2[iPin].lpMediaType);
                        }
                    }

                    CoTaskMemFree((void*)rgf2.u.s2.rgPins2);
                }
            }
        }
    }

    if (hkeyFilter) RegCloseKey(hkeyFilter);

    if (pMapper)
        IFilterMapper2_Release(pMapper);

    return S_OK;
}

/**********************************************************************
 * DEVENUM_ICreateDevEnum_CreateClassEnumerator
 */
static HRESULT WINAPI DEVENUM_ICreateDevEnum_CreateClassEnumerator(
    ICreateDevEnum * iface,
    REFCLSID clsidDeviceClass,
    IEnumMoniker **ppEnumMoniker,
    DWORD dwFlags)
{
    TRACE("(%p)->(%s, %p, %x)\n", iface, debugstr_guid(clsidDeviceClass), ppEnumMoniker, dwFlags);

    if (!ppEnumMoniker)
        return E_POINTER;

    *ppEnumMoniker = NULL;

    register_codecs();
    DEVENUM_RegisterLegacyAmFilters();

    return create_EnumMoniker(clsidDeviceClass, ppEnumMoniker);
}

/**********************************************************************
 * ICreateDevEnum_Vtbl
 */
static const ICreateDevEnumVtbl ICreateDevEnum_Vtbl =
{
    DEVENUM_ICreateDevEnum_QueryInterface,
    DEVENUM_ICreateDevEnum_AddRef,
    DEVENUM_ICreateDevEnum_Release,
    DEVENUM_ICreateDevEnum_CreateClassEnumerator,
};

/**********************************************************************
 * static CreateDevEnum instance
 */
ICreateDevEnum DEVENUM_CreateDevEnum = { &ICreateDevEnum_Vtbl };

/**********************************************************************
 * DEVENUM_CreateAMCategoryKey (INTERNAL)
 *
 * Creates a registry key for a category at HKEY_CURRENT_USER\Software\
 * Microsoft\ActiveMovie\devenum\{clsid}
 */
static HRESULT DEVENUM_CreateAMCategoryKey(const CLSID * clsidCategory)
{
    WCHAR wszRegKey[MAX_PATH];
    HRESULT res = S_OK;
    HKEY hkeyDummy = NULL;

    strcpyW(wszRegKey, wszActiveMovieKey);

    if (!StringFromGUID2(clsidCategory, wszRegKey + strlenW(wszRegKey), sizeof(wszRegKey)/sizeof(wszRegKey[0]) - strlenW(wszRegKey)))
        res = E_INVALIDARG;

    if (SUCCEEDED(res))
    {
        LONG lRes = RegCreateKeyW(HKEY_CURRENT_USER, wszRegKey, &hkeyDummy);
        res = HRESULT_FROM_WIN32(lRes);
    }

    if (hkeyDummy)
        RegCloseKey(hkeyDummy);

    if (FAILED(res))
        ERR("Failed to create key HKEY_CURRENT_USER\\%s\n", debugstr_w(wszRegKey));

    return res;
}

static void register_vfw_codecs(void)
{
    WCHAR avico_clsid_str[CHARS_IN_GUID];
    HKEY basekey, key;
    ICINFO icinfo;
    DWORD i, res;

    static const WCHAR CLSIDW[] = {'C','L','S','I','D',0};
    static const WCHAR FccHandlerW[] = {'F','c','c','H','a','n','d','l','e','r',0};
    static const WCHAR FriendlyNameW[] = {'F','r','i','e','n','d','l','y','N','a','m','e',0};

    StringFromGUID2(&CLSID_AVICo, avico_clsid_str, sizeof(avico_clsid_str)/sizeof(WCHAR));

    basekey = open_special_category_key(&CLSID_VideoCompressorCategory, TRUE);
    if(!basekey) {
        ERR("Could not create key\n");
        return;
    }

    for(i=0; ICInfo(FCC('v','i','d','c'), i, &icinfo); i++) {
        WCHAR fcc_str[5] = {LOBYTE(LOWORD(icinfo.fccHandler)), HIBYTE(LOWORD(icinfo.fccHandler)),
                            LOBYTE(HIWORD(icinfo.fccHandler)), HIBYTE(HIWORD(icinfo.fccHandler))};

        res = RegCreateKeyW(basekey, fcc_str, &key);
        if(res != ERROR_SUCCESS)
            continue;

        RegSetValueExW(key, CLSIDW, 0, REG_SZ, (const BYTE*)avico_clsid_str, sizeof(avico_clsid_str));
        RegSetValueExW(key, FccHandlerW, 0, REG_SZ, (const BYTE*)fcc_str, sizeof(fcc_str));
        RegSetValueExW(key, FriendlyNameW, 0, REG_SZ, (const BYTE*)icinfo.szName, (strlenW(icinfo.szName)+1)*sizeof(WCHAR));
        /* FIXME: Set ClassManagerFlags and FilterData values */

        RegCloseKey(key);
    }

    RegCloseKey(basekey);
}

static HRESULT register_codecs(void)
{
    HRESULT res;
    WCHAR szDSoundNameFormat[MAX_PATH + 1];
    WCHAR szDSoundName[MAX_PATH + 1];
    WCHAR class[CHARS_IN_GUID];
    DWORD iDefaultDevice = -1;
    UINT numDevs;
    IFilterMapper2 * pMapper = NULL;
    REGFILTER2 rf2;
    REGFILTERPINS2 rfp2;
    HKEY basekey;

    /* Since devices can change between session, for example because you just plugged in a webcam
     * or switched from pulseaudio to alsa, delete all old devices first
     */
    RegOpenKeyW(HKEY_CURRENT_USER, wszActiveMovieKey, &basekey);
    StringFromGUID2(&CLSID_AudioRendererCategory, class, CHARS_IN_GUID);
    RegDeleteTreeW(basekey, class);
    StringFromGUID2(&CLSID_AudioInputDeviceCategory, class, CHARS_IN_GUID);
    RegDeleteTreeW(basekey, class);
    StringFromGUID2(&CLSID_VideoInputDeviceCategory, class, CHARS_IN_GUID);
    RegDeleteTreeW(basekey, class);
    StringFromGUID2(&CLSID_MidiRendererCategory, class, CHARS_IN_GUID);
    RegDeleteTreeW(basekey, class);
    StringFromGUID2(&CLSID_VideoCompressorCategory, class, CHARS_IN_GUID);
    RegDeleteTreeW(basekey, class);
    RegCloseKey(basekey);

    rf2.dwVersion = 2;
    rf2.dwMerit = MERIT_PREFERRED;
    rf2.u.s2.cPins2 = 1;
    rf2.u.s2.rgPins2 = &rfp2;
    rfp2.cInstances = 1;
    rfp2.nMediums = 0;
    rfp2.lpMedium = NULL;
    rfp2.clsPinCategory = &IID_NULL;

    if (!LoadStringW(DEVENUM_hInstance, IDS_DEVENUM_DS, szDSoundNameFormat, sizeof(szDSoundNameFormat)/sizeof(szDSoundNameFormat[0])-1))
    {
        ERR("Couldn't get string resource (GetLastError() is %d)\n", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    res = CoCreateInstance(&CLSID_FilterMapper2, NULL, CLSCTX_INPROC,
                           &IID_IFilterMapper2, (void **) &pMapper);
    /*
     * Fill in info for devices
     */
    if (SUCCEEDED(res))
    {
        UINT i;
        WAVEOUTCAPSW wocaps;
	WAVEINCAPSW wicaps;
        MIDIOUTCAPSW mocaps;
        REGPINTYPES * pTypes;
        IPropertyBag * pPropBag = NULL;

	numDevs = waveOutGetNumDevs();

        res = DEVENUM_CreateAMCategoryKey(&CLSID_AudioRendererCategory);
        if (FAILED(res)) /* can't register any devices in this category */
            numDevs = 0;

	rfp2.dwFlags = REG_PINFLAG_B_RENDERER;
	for (i = 0; i < numDevs; i++)
	{
	    if (waveOutGetDevCapsW(i, &wocaps, sizeof(WAVEOUTCAPSW))
	        == MMSYSERR_NOERROR)
	    {
                IMoniker * pMoniker = NULL;

                rfp2.nMediaTypes = 1;
                pTypes = CoTaskMemAlloc(rfp2.nMediaTypes * sizeof(REGPINTYPES));
                if (!pTypes)
                {
                    IFilterMapper2_Release(pMapper);
                    return E_OUTOFMEMORY;
                }
                /* FIXME: Native devenum seems to register a lot more types for
                 * DSound than we do. Not sure what purpose they serve */
                pTypes[0].clsMajorType = &MEDIATYPE_Audio;
                pTypes[0].clsMinorType = &MEDIASUBTYPE_PCM;

                rfp2.lpMediaType = pTypes;

                res = IFilterMapper2_RegisterFilter(pMapper,
		                              &CLSID_AudioRender,
					      wocaps.szPname,
					      &pMoniker,
					      &CLSID_AudioRendererCategory,
					      wocaps.szPname,
					      &rf2);

                if (pMoniker)
                {
                    VARIANT var;

                    V_VT(&var) = VT_I4;
                    V_I4(&var) = i;
                    res = IMoniker_BindToStorage(pMoniker, NULL, NULL, &IID_IPropertyBag, (LPVOID)&pPropBag);
                    if (SUCCEEDED(res))
                        res = IPropertyBag_Write(pPropBag, wszWaveOutID, &var);
                    else
                        pPropBag = NULL;

                    V_VT(&var) = VT_LPWSTR;
                    V_BSTR(&var) = wocaps.szPname;
                    if (SUCCEEDED(res))
                        res = IPropertyBag_Write(pPropBag, wszFriendlyName, &var);
                    if (pPropBag)
                        IPropertyBag_Release(pPropBag);
                    IMoniker_Release(pMoniker);
                    pMoniker = NULL;
                }

		wsprintfW(szDSoundName, szDSoundNameFormat, wocaps.szPname);
	        res = IFilterMapper2_RegisterFilter(pMapper,
		                              &CLSID_DSoundRender,
					      szDSoundName,
					      &pMoniker,
					      &CLSID_AudioRendererCategory,
					      szDSoundName,
					      &rf2);

                /* FIXME: do additional stuff with IMoniker here, depending on what RegisterFilter does */

		if (pMoniker)
		    IMoniker_Release(pMoniker);

		if (i == iDefaultDevice)
		{
		    FIXME("Default device\n");
		}

                CoTaskMemFree(pTypes);
	    }
	}

        numDevs = waveInGetNumDevs();

        res = DEVENUM_CreateAMCategoryKey(&CLSID_AudioInputDeviceCategory);
        if (FAILED(res)) /* can't register any devices in this category */
            numDevs = 0;

	rfp2.dwFlags = REG_PINFLAG_B_OUTPUT;
        for (i = 0; i < numDevs; i++)
        {
            if (waveInGetDevCapsW(i, &wicaps, sizeof(WAVEINCAPSW))
                == MMSYSERR_NOERROR)
            {
                IMoniker * pMoniker = NULL;

                rfp2.nMediaTypes = 1;
                pTypes = CoTaskMemAlloc(rfp2.nMediaTypes * sizeof(REGPINTYPES));
                if (!pTypes)
                {
                    IFilterMapper2_Release(pMapper);
                    return E_OUTOFMEMORY;
                }

                /* FIXME: Not sure if these are correct */
                pTypes[0].clsMajorType = &MEDIATYPE_Audio;
                pTypes[0].clsMinorType = &MEDIASUBTYPE_PCM;

                rfp2.lpMediaType = pTypes;

                res = IFilterMapper2_RegisterFilter(pMapper,
		                              &CLSID_AudioRecord,
					      wicaps.szPname,
					      &pMoniker,
					      &CLSID_AudioInputDeviceCategory,
					      wicaps.szPname,
					      &rf2);


                if (pMoniker) {
                    VARIANT var;

                    V_VT(&var) = VT_I4;
                    V_I4(&var) = i;
                    res = IMoniker_BindToStorage(pMoniker, NULL, NULL, &IID_IPropertyBag, (LPVOID)&pPropBag);
                    if (SUCCEEDED(res))
                        res = IPropertyBag_Write(pPropBag, wszWaveInID, &var);
                    else
                        pPropBag = NULL;

                    V_VT(&var) = VT_LPWSTR;
                    V_BSTR(&var) = wicaps.szPname;
                    if (SUCCEEDED(res))
                        res = IPropertyBag_Write(pPropBag, wszFriendlyName, &var);

                    if (pPropBag)
                        IPropertyBag_Release(pPropBag);
                    IMoniker_Release(pMoniker);
                }

                CoTaskMemFree(pTypes);
	    }
	}

	numDevs = midiOutGetNumDevs();

        res = DEVENUM_CreateAMCategoryKey(&CLSID_MidiRendererCategory);
        if (FAILED(res)) /* can't register any devices in this category */
            numDevs = 0;

	rfp2.dwFlags = REG_PINFLAG_B_RENDERER;
	for (i = 0; i < numDevs; i++)
	{
	    if (midiOutGetDevCapsW(i, &mocaps, sizeof(MIDIOUTCAPSW))
	        == MMSYSERR_NOERROR)
	    {
                IMoniker * pMoniker = NULL;

                rfp2.nMediaTypes = 1;
                pTypes = CoTaskMemAlloc(rfp2.nMediaTypes * sizeof(REGPINTYPES));
                if (!pTypes)
                {
                    IFilterMapper2_Release(pMapper);
                    return E_OUTOFMEMORY;
                }

                /* FIXME: Not sure if these are correct */
                pTypes[0].clsMajorType = &MEDIATYPE_Midi;
                pTypes[0].clsMinorType = &MEDIASUBTYPE_None;

                rfp2.lpMediaType = pTypes;

                res = IFilterMapper2_RegisterFilter(pMapper,
		                              &CLSID_AVIMIDIRender,
					      mocaps.szPname,
					      &pMoniker,
					      &CLSID_MidiRendererCategory,
					      mocaps.szPname,
					      &rf2);

                /* FIXME: do additional stuff with IMoniker here, depending on what RegisterFilter does */
		/* Native version sets MidiOutId */

		if (pMoniker)
		    IMoniker_Release(pMoniker);

		if (i == iDefaultDevice)
		{
		    FIXME("Default device\n");
		}

                CoTaskMemFree(pTypes);
	    }
	}
        res = DEVENUM_CreateAMCategoryKey(&CLSID_VideoInputDeviceCategory);
        if (SUCCEEDED(res))
            for (i = 0; i < 10; i++)
            {
                WCHAR szDeviceName[32], szDeviceVersion[32], szDevicePath[10];

                if (capGetDriverDescriptionW ((WORD) i,
                                              szDeviceName, sizeof(szDeviceName)/sizeof(WCHAR),
                                              szDeviceVersion, sizeof(szDeviceVersion)/sizeof(WCHAR)))
                {
                    IMoniker * pMoniker = NULL;
                    WCHAR dprintf[] = { 'v','i','d','e','o','%','d',0 };
                    snprintfW(szDevicePath, sizeof(szDevicePath)/sizeof(WCHAR), dprintf, i);
                    /* The above code prevents 1 device with a different ID overwriting another */

                    rfp2.nMediaTypes = 1;
                    pTypes = CoTaskMemAlloc(rfp2.nMediaTypes * sizeof(REGPINTYPES));
                    if (!pTypes) {
                        IFilterMapper2_Release(pMapper);
                        return E_OUTOFMEMORY;
                    }

                    pTypes[0].clsMajorType = &MEDIATYPE_Video;
                    pTypes[0].clsMinorType = &MEDIASUBTYPE_None;

                    rfp2.lpMediaType = pTypes;

                    res = IFilterMapper2_RegisterFilter(pMapper,
                                                        &CLSID_VfwCapture,
                                                        szDeviceName,
                                                        &pMoniker,
                                                        &CLSID_VideoInputDeviceCategory,
                                                        szDevicePath,
                                                        &rf2);

                    if (pMoniker) {
                       OLECHAR wszVfwIndex[] = { 'V','F','W','I','n','d','e','x',0 };
                       VARIANT var;
                       V_VT(&var) = VT_I4;
                       V_I4(&var) = i;
                       res = IMoniker_BindToStorage(pMoniker, NULL, NULL, &IID_IPropertyBag, (LPVOID)&pPropBag);
                       if (SUCCEEDED(res)) {
                           res = IPropertyBag_Write(pPropBag, wszVfwIndex, &var);
                           IPropertyBag_Release(pPropBag);
                       }
                       IMoniker_Release(pMoniker);
                    }

                    if (i == iDefaultDevice) FIXME("Default device\n");
                    CoTaskMemFree(pTypes);
                }
            }
    }

    if (pMapper)
        IFilterMapper2_Release(pMapper);

    register_vfw_codecs();

    return res;
}
