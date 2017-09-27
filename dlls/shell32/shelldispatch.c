/*
 * IShellDispatch implementation
 *
 * Copyright 2010 Alexander Morozov for Etersoft
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
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winsvc.h"
#include "shlwapi.h"
#include "shlobj.h"
#include "shldisp.h"
#include "debughlp.h"

#include "shell32_main.h"
#include "pidl.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

static ITypeLib *typelib;
static const IID * const tid_ids[] =
{
    &IID_NULL,
    &IID_IShellDispatch6,
    &IID_IShellFolderViewDual3,
    &IID_Folder3,
    &IID_FolderItem2,
    &IID_FolderItems3,
    &IID_FolderItemVerb,
    &IID_FolderItemVerbs
};
static ITypeInfo *typeinfos[LAST_tid];

typedef struct {
    IShellDispatch6 IShellDispatch6_iface;
    LONG ref;
} ShellDispatch;

typedef struct {
    Folder3 Folder3_iface;
    LONG ref;
    VARIANT dir;
    IDispatch *application;
    IShellFolder2 *folder;
    PIDLIST_ABSOLUTE pidl;
    WCHAR path[MAX_PATH];
} FolderImpl;

typedef struct {
    FolderItems3 FolderItems3_iface;
    LONG ref;
    FolderImpl *folder;
    WCHAR **item_names;
    LONG item_count;
} FolderItemsImpl;

typedef struct {
    FolderItem2 FolderItem2_iface;
    LONG ref;
    FolderImpl *folder;
    WCHAR *path; /* if NULL, folder path is used */
    DWORD attributes;
} FolderItemImpl;

typedef struct {
    FolderItemVerbs FolderItemVerbs_iface;
    LONG ref;

    IContextMenu *contextmenu;
    HMENU hMenu;
    LONG count;
} FolderItemVerbsImpl;

typedef struct {
    FolderItemVerb FolderItemVerb_iface;
    LONG ref;

    IContextMenu *contextmenu;
    BSTR name;
} FolderItemVerbImpl;

static inline ShellDispatch *impl_from_IShellDispatch6(IShellDispatch6 *iface)
{
    return CONTAINING_RECORD(iface, ShellDispatch, IShellDispatch6_iface);
}

static inline FolderImpl *impl_from_Folder(Folder3 *iface)
{
    return CONTAINING_RECORD(iface, FolderImpl, Folder3_iface);
}

static inline FolderItemsImpl *impl_from_FolderItems(FolderItems3 *iface)
{
    return CONTAINING_RECORD(iface, FolderItemsImpl, FolderItems3_iface);
}

static inline FolderItemImpl *impl_from_FolderItem(FolderItem2 *iface)
{
    return CONTAINING_RECORD(iface, FolderItemImpl, FolderItem2_iface);
}

static inline FolderItemVerbsImpl *impl_from_FolderItemVerbs(FolderItemVerbs *iface)
{
    return CONTAINING_RECORD(iface, FolderItemVerbsImpl, FolderItemVerbs_iface);
}

static inline FolderItemVerbImpl *impl_from_FolderItemVerb(FolderItemVerb *iface)
{
    return CONTAINING_RECORD(iface, FolderItemVerbImpl, FolderItemVerb_iface);
}

static HRESULT load_typelib(void)
{
    ITypeLib *tl;
    HRESULT hr;

    hr = LoadRegTypeLib(&LIBID_Shell32, 1, 0, LOCALE_SYSTEM_DEFAULT, &tl);
    if (FAILED(hr)) {
        ERR("LoadRegTypeLib failed: %08x\n", hr);
        return hr;
    }

    if (InterlockedCompareExchangePointer((void**)&typelib, tl, NULL))
        ITypeLib_Release(tl);
    return hr;
}

void release_typelib(void)
{
    unsigned i;

    if (!typelib)
        return;

    for (i = 0; i < sizeof(typeinfos)/sizeof(*typeinfos); i++)
        if (typeinfos[i])
            ITypeInfo_Release(typeinfos[i]);

    ITypeLib_Release(typelib);
}

HRESULT get_typeinfo(enum tid_t tid, ITypeInfo **typeinfo)
{
    HRESULT hr;

    if (!typelib)
        hr = load_typelib();
    if (!typelib)
        return hr;

    if (!typeinfos[tid])
    {
        ITypeInfo *ti;

        hr = ITypeLib_GetTypeInfoOfGuid(typelib, tid_ids[tid], &ti);
        if (FAILED(hr))
        {
            ERR("GetTypeInfoOfGuid(%s) failed: %08x\n", debugstr_guid(tid_ids[tid]), hr);
            return hr;
        }

        if (InterlockedCompareExchangePointer((void**)(typeinfos+tid), ti, NULL))
            ITypeInfo_Release(ti);
    }

    *typeinfo = typeinfos[tid];
    return S_OK;
}

/* FolderItemVerb */
static HRESULT WINAPI FolderItemVerbImpl_QueryInterface(FolderItemVerb *iface,
    REFIID riid, void **ppv)
{
    FolderItemVerbImpl *This = impl_from_FolderItemVerb(iface);

    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IDispatch, riid) ||
        IsEqualIID(&IID_FolderItemVerb, riid))
        *ppv = &This->FolderItemVerb_iface;
    else
    {
        WARN("not implemented for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI FolderItemVerbImpl_AddRef(FolderItemVerb *iface)
{
    FolderItemVerbImpl *This = impl_from_FolderItemVerb(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    return ref;
}

static ULONG WINAPI FolderItemVerbImpl_Release(FolderItemVerb *iface)
{
    FolderItemVerbImpl *This = impl_from_FolderItemVerb(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    if (!ref)
    {
        IContextMenu_Release(This->contextmenu);
        SysFreeString(This->name);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

static HRESULT WINAPI FolderItemVerbImpl_GetTypeInfoCount(FolderItemVerb *iface, UINT *pctinfo)
{
    TRACE("(%p,%p)\n", iface, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI FolderItemVerbImpl_GetTypeInfo(FolderItemVerb *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HRESULT hr;

    TRACE("(%p,%u,%d,%p)\n", iface, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(FolderItemVerb_tid, ppTInfo);
    if (SUCCEEDED(hr))
        ITypeInfo_AddRef(*ppTInfo);
    return hr;
}

static HRESULT WINAPI FolderItemVerbImpl_GetIDsOfNames(FolderItemVerb *iface,
        REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%s,%p,%u,%d,%p)\n", iface, shdebugstr_guid(riid), rgszNames, cNames, lcid,
            rgDispId);

    hr = get_typeinfo(FolderItemVerb_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_GetIDsOfNames(ti, rgszNames, cNames, rgDispId);
    return hr;
}

static HRESULT WINAPI FolderItemVerbImpl_Invoke(FolderItemVerb *iface,
        DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
        DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo,
        UINT *puArgErr)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%d,%s,%d,%u,%p,%p,%p,%p)\n", iface, dispIdMember, shdebugstr_guid(riid), lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(FolderItemVerb_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_Invoke(ti, iface, dispIdMember, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
    return hr;
}

static HRESULT WINAPI FolderItemVerbImpl_get_Application(FolderItemVerb *iface, IDispatch **disp)
{
    TRACE("(%p, %p)\n", iface, disp);

    if (disp)
        *disp = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemVerbImpl_get_Parent(FolderItemVerb *iface, IDispatch **disp)
{
    TRACE("(%p, %p)\n", iface, disp);

    if (disp)
        *disp = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemVerbImpl_get_Name(FolderItemVerb *iface, BSTR *name)
{
    FolderItemVerbImpl *This = impl_from_FolderItemVerb(iface);

    TRACE("(%p, %p)\n", iface, name);

    *name = SysAllocString(This->name);
    return *name ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI FolderItemVerbImpl_DoIt(FolderItemVerb *iface)
{
    FIXME("(%p)\n", iface);
    return E_NOTIMPL;
}

static FolderItemVerbVtbl folderitemverbvtbl = {
    FolderItemVerbImpl_QueryInterface,
    FolderItemVerbImpl_AddRef,
    FolderItemVerbImpl_Release,
    FolderItemVerbImpl_GetTypeInfoCount,
    FolderItemVerbImpl_GetTypeInfo,
    FolderItemVerbImpl_GetIDsOfNames,
    FolderItemVerbImpl_Invoke,
    FolderItemVerbImpl_get_Application,
    FolderItemVerbImpl_get_Parent,
    FolderItemVerbImpl_get_Name,
    FolderItemVerbImpl_DoIt
};

static HRESULT FolderItemVerb_Constructor(IContextMenu *contextmenu, BSTR name, FolderItemVerb **verb)
{
    FolderItemVerbImpl *This;

    TRACE("%p, %s\n", contextmenu, debugstr_w(name));

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(FolderItemVerbImpl));
    if (!This)
        return E_OUTOFMEMORY;

    This->FolderItemVerb_iface.lpVtbl = &folderitemverbvtbl;
    This->ref = 1;
    This->contextmenu = contextmenu;
    IContextMenu_AddRef(contextmenu);
    This->name = name;

    *verb = &This->FolderItemVerb_iface;
    return S_OK;
}

/* FolderItemVerbs */
static HRESULT WINAPI FolderItemVerbsImpl_QueryInterface(FolderItemVerbs *iface,
    REFIID riid, void **ppv)
{
    FolderItemVerbsImpl *This = impl_from_FolderItemVerbs(iface);

    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IDispatch, riid) ||
        IsEqualIID(&IID_FolderItemVerbs, riid))
        *ppv = &This->FolderItemVerbs_iface;
    else
    {
        WARN("not implemented for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI FolderItemVerbsImpl_AddRef(FolderItemVerbs *iface)
{
    FolderItemVerbsImpl *This = impl_from_FolderItemVerbs(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    return ref;
}

static ULONG WINAPI FolderItemVerbsImpl_Release(FolderItemVerbs *iface)
{
    FolderItemVerbsImpl *This = impl_from_FolderItemVerbs(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    if (!ref)
    {
        IContextMenu_Release(This->contextmenu);
        DestroyMenu(This->hMenu);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

static HRESULT WINAPI FolderItemVerbsImpl_GetTypeInfoCount(FolderItemVerbs *iface, UINT *pctinfo)
{
    TRACE("(%p,%p)\n", iface, pctinfo);
    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI FolderItemVerbsImpl_GetTypeInfo(FolderItemVerbs *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HRESULT hr;

    TRACE("(%p,%u,%d,%p)\n", iface, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(FolderItemVerbs_tid, ppTInfo);
    if (SUCCEEDED(hr))
        ITypeInfo_AddRef(*ppTInfo);
    return hr;
}

static HRESULT WINAPI FolderItemVerbsImpl_GetIDsOfNames(FolderItemVerbs *iface,
        REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%s,%p,%u,%d,%p)\n", iface, shdebugstr_guid(riid), rgszNames, cNames, lcid,
            rgDispId);

    hr = get_typeinfo(FolderItemVerbs_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_GetIDsOfNames(ti, rgszNames, cNames, rgDispId);
    return hr;
}

static HRESULT WINAPI FolderItemVerbsImpl_Invoke(FolderItemVerbs *iface,
        DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
        DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo,
        UINT *puArgErr)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%d,%s,%d,%u,%p,%p,%p,%p)\n", iface, dispIdMember, shdebugstr_guid(riid), lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(FolderItemVerbs_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_Invoke(ti, iface, dispIdMember, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
    return hr;
}

static HRESULT WINAPI FolderItemVerbsImpl_get_Count(FolderItemVerbs *iface, LONG *count)
{
    FolderItemVerbsImpl *This = impl_from_FolderItemVerbs(iface);

    TRACE("(%p, %p)\n", iface, count);

    if (!count)
        return E_INVALIDARG;

    *count = This->count;
    return S_OK;
}

static HRESULT WINAPI FolderItemVerbsImpl_get_Application(FolderItemVerbs *iface, IDispatch **disp)
{
    TRACE("(%p, %p)\n", iface, disp);

    if (disp)
        *disp = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemVerbsImpl_get_Parent(FolderItemVerbs *iface, IDispatch **disp)
{
    TRACE("(%p, %p)\n", iface, disp);

    if (disp)
        *disp = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemVerbsImpl_Item(FolderItemVerbs *iface, VARIANT index, FolderItemVerb **verb)
{
    FolderItemVerbsImpl *This = impl_from_FolderItemVerbs(iface);
    MENUITEMINFOW info;
    HRESULT hr;
    VARIANT v;
    BSTR name;

    TRACE("(%p, %s, %p)\n", iface, debugstr_variant(&index), verb);

    if (!verb)
        return E_INVALIDARG;

    *verb = NULL;

    VariantInit(&v);
    VariantCopyInd(&v, &index);

    hr = VariantChangeType(&v, &v, 0, VT_I4);
    if (FAILED(hr))
    {
        FIXME("failed to coerce to VT_I4, %s\n", debugstr_variant(&v));
        return hr;
    }

    if (V_I4(&v) > This->count)
        return S_OK;

    if (V_I4(&v) == This->count)
        name = SysAllocStringLen(NULL, 0);
    else
    {
        /* get item name */
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask = MIIM_STRING;
        if (!GetMenuItemInfoW(This->hMenu, V_I4(&v), TRUE, &info))
            return E_FAIL;

        name = SysAllocStringLen(NULL, info.cch);
        if (name)
        {
            info.dwTypeData = name;
            info.cch++;
            GetMenuItemInfoW(This->hMenu, V_I4(&v), TRUE, &info);
        }
    }

    if (!name)
        return E_OUTOFMEMORY;

    return FolderItemVerb_Constructor(This->contextmenu, name, verb);
}

static HRESULT WINAPI FolderItemVerbsImpl__NewEnum(FolderItemVerbs *iface, IUnknown **ret)
{
    FIXME("(%p, %p)\n", iface, ret);
    return E_NOTIMPL;
}

static FolderItemVerbsVtbl folderitemverbsvtbl = {
    FolderItemVerbsImpl_QueryInterface,
    FolderItemVerbsImpl_AddRef,
    FolderItemVerbsImpl_Release,
    FolderItemVerbsImpl_GetTypeInfoCount,
    FolderItemVerbsImpl_GetTypeInfo,
    FolderItemVerbsImpl_GetIDsOfNames,
    FolderItemVerbsImpl_Invoke,
    FolderItemVerbsImpl_get_Count,
    FolderItemVerbsImpl_get_Application,
    FolderItemVerbsImpl_get_Parent,
    FolderItemVerbsImpl_Item,
    FolderItemVerbsImpl__NewEnum
};

static HRESULT FolderItemVerbs_Constructor(BSTR path, FolderItemVerbs **verbs)
{
    FolderItemVerbsImpl *This;
    IShellFolder *folder;
    LPCITEMIDLIST child;
    LPITEMIDLIST pidl;
    HRESULT hr;

    *verbs = NULL;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(FolderItemVerbsImpl));
    if (!This)
        return E_OUTOFMEMORY;

    This->FolderItemVerbs_iface.lpVtbl = &folderitemverbsvtbl;
    This->ref = 1;

    /* build context menu for this path */
    hr = SHParseDisplayName(path, NULL, &pidl, 0, NULL);
    if (FAILED(hr))
        goto failed;

    hr = SHBindToParent(pidl, &IID_IShellFolder, (void**)&folder, &child);
    CoTaskMemFree(pidl);
    if (FAILED(hr))
        goto failed;

    hr = IShellFolder_GetUIObjectOf(folder, NULL, 1, &child, &IID_IContextMenu, NULL, (void**)&This->contextmenu);
    IShellFolder_Release(folder);
    if (FAILED(hr))
        goto failed;

    This->hMenu = CreatePopupMenu();
    hr = IContextMenu_QueryContextMenu(This->contextmenu, This->hMenu, 0, FCIDM_SHVIEWFIRST, FCIDM_SHVIEWLAST, CMF_NORMAL);
    if (FAILED(hr))
    {
        FolderItemVerbs_Release(&This->FolderItemVerbs_iface);
        return hr;
    }

    This->count = GetMenuItemCount(This->hMenu);
    *verbs = &This->FolderItemVerbs_iface;
    return S_OK;

failed:
    HeapFree(GetProcessHeap(), 0, This);
    return hr;
}

static HRESULT WINAPI FolderItemImpl_QueryInterface(FolderItem2 *iface,
        REFIID riid, LPVOID *ppv)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IDispatch, riid) ||
        IsEqualIID(&IID_FolderItem, riid) ||
        IsEqualIID(&IID_FolderItem2, riid))
        *ppv = &This->FolderItem2_iface;
    else
    {
        WARN("not implemented for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI FolderItemImpl_AddRef(FolderItem2 *iface)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    return ref;
}

static ULONG WINAPI FolderItemImpl_Release(FolderItem2 *iface)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    if (!ref)
    {
        Folder3_Release(&This->folder->Folder3_iface);
        HeapFree(GetProcessHeap(), 0, This->path);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static HRESULT WINAPI FolderItemImpl_GetTypeInfoCount(FolderItem2 *iface,
        UINT *pctinfo)
{
    TRACE("(%p,%p)\n", iface, pctinfo);

    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI FolderItemImpl_GetTypeInfo(FolderItem2 *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HRESULT hr;

    TRACE("(%p,%u,%d,%p)\n", iface, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(FolderItem2_tid, ppTInfo);
    if (SUCCEEDED(hr))
        ITypeInfo_AddRef(*ppTInfo);
    return hr;
}

static HRESULT WINAPI FolderItemImpl_GetIDsOfNames(FolderItem2 *iface,
        REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid,
        DISPID *rgDispId)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%s,%p,%u,%d,%p)\n", iface, shdebugstr_guid(riid), rgszNames, cNames, lcid,
            rgDispId);

    hr = get_typeinfo(FolderItem2_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_GetIDsOfNames(ti, rgszNames, cNames, rgDispId);
    return hr;
}

static HRESULT WINAPI FolderItemImpl_Invoke(FolderItem2 *iface,
        DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
        DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo,
        UINT *puArgErr)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%d,%s,%d,%u,%p,%p,%p,%p)\n", iface, dispIdMember, shdebugstr_guid(riid), lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(FolderItem2_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_Invoke(ti, This, dispIdMember, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
    return hr;
}

static HRESULT WINAPI FolderItemImpl_get_Application(FolderItem2 *iface, IDispatch **disp)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, disp);

    return Folder3_get_Application(&This->folder->Folder3_iface, disp);
}

static HRESULT WINAPI FolderItemImpl_get_Parent(FolderItem2 *iface, IDispatch **disp)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, disp);

    if (disp)
    {
        *disp = (IDispatch *)&This->folder->Folder3_iface;
        IDispatch_AddRef(*disp);
    }

    return S_OK;
}

static HRESULT WINAPI FolderItemImpl_get_Name(FolderItem2 *iface, BSTR *pbs)
{
    FIXME("(%p,%p)\n", iface, pbs);

    *pbs = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_put_Name(FolderItem2 *iface, BSTR bs)
{
    FIXME("(%p,%s)\n", iface, debugstr_w(bs));

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_get_Path(FolderItem2 *iface, BSTR *path)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, path);

    *path = SysAllocString(This->path ? This->path : This->folder->path);
    return *path ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI FolderItemImpl_get_GetLink(FolderItem2 *iface,
        IDispatch **ppid)
{
    FIXME("(%p,%p)\n", iface, ppid);

    *ppid = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_get_GetFolder(FolderItem2 *iface,
        IDispatch **ppid)
{
    FIXME("(%p,%p)\n", iface, ppid);

    *ppid = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_get_IsLink(FolderItem2 *iface, VARIANT_BOOL *b)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, b);

    *b = This->attributes & SFGAO_LINK ? VARIANT_TRUE : VARIANT_FALSE;

    return S_OK;
}

static HRESULT WINAPI FolderItemImpl_get_IsFolder(FolderItem2 *iface, VARIANT_BOOL *b)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, b);

    *b = This->attributes & SFGAO_FOLDER ? VARIANT_TRUE : VARIANT_FALSE;

    return S_OK;
}

static HRESULT WINAPI FolderItemImpl_get_IsFileSystem(FolderItem2 *iface, VARIANT_BOOL *b)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, b);

    *b = This->attributes & SFGAO_FILESYSTEM ? VARIANT_TRUE : VARIANT_FALSE;

    return S_OK;
}

static HRESULT WINAPI FolderItemImpl_get_IsBrowsable(FolderItem2 *iface, VARIANT_BOOL *b)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p,%p)\n", iface, b);

    *b = This->attributes & SFGAO_BROWSABLE ? VARIANT_TRUE : VARIANT_FALSE;

    return S_OK;
}

static HRESULT WINAPI FolderItemImpl_get_ModifyDate(FolderItem2 *iface,
        DATE *pdt)
{
    FIXME("(%p,%p)\n", iface, pdt);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_put_ModifyDate(FolderItem2 *iface, DATE dt)
{
    FIXME("(%p,%f)\n", iface, dt);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_get_Size(FolderItem2 *iface, LONG *pul)
{
    FIXME("(%p,%p)\n", iface, pul);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_get_Type(FolderItem2 *iface, BSTR *pbs)
{
    FIXME("(%p,%p)\n", iface, pbs);

    *pbs = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_Verbs(FolderItem2 *iface, FolderItemVerbs **verbs)
{
    FolderItemImpl *This = impl_from_FolderItem(iface);

    TRACE("(%p, %p)\n", iface, verbs);

    if (!verbs)
        return E_INVALIDARG;

    return FolderItemVerbs_Constructor(This->path ? This->path : This->folder->path, verbs);
}

static HRESULT WINAPI FolderItemImpl_InvokeVerb(FolderItem2 *iface,
        VARIANT vVerb)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_InvokeVerbEx(FolderItem2 *iface, VARIANT verb, VARIANT args)
{
    FIXME("(%p): stub\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemImpl_ExtendedProperty(FolderItem2 *iface, BSTR propname, VARIANT *ret)
{
    FIXME("(%p)->(%s %p): stub\n", iface, debugstr_w(propname), ret);

    return E_NOTIMPL;
}

static const FolderItem2Vtbl FolderItemImpl_Vtbl = {
    FolderItemImpl_QueryInterface,
    FolderItemImpl_AddRef,
    FolderItemImpl_Release,
    FolderItemImpl_GetTypeInfoCount,
    FolderItemImpl_GetTypeInfo,
    FolderItemImpl_GetIDsOfNames,
    FolderItemImpl_Invoke,
    FolderItemImpl_get_Application,
    FolderItemImpl_get_Parent,
    FolderItemImpl_get_Name,
    FolderItemImpl_put_Name,
    FolderItemImpl_get_Path,
    FolderItemImpl_get_GetLink,
    FolderItemImpl_get_GetFolder,
    FolderItemImpl_get_IsLink,
    FolderItemImpl_get_IsFolder,
    FolderItemImpl_get_IsFileSystem,
    FolderItemImpl_get_IsBrowsable,
    FolderItemImpl_get_ModifyDate,
    FolderItemImpl_put_ModifyDate,
    FolderItemImpl_get_Size,
    FolderItemImpl_get_Type,
    FolderItemImpl_Verbs,
    FolderItemImpl_InvokeVerb,
    FolderItemImpl_InvokeVerbEx,
    FolderItemImpl_ExtendedProperty
};

static HRESULT FolderItem_Constructor(FolderImpl *folder, const WCHAR *path, FolderItem **item)
{
    PIDLIST_ABSOLUTE pidl;
    FolderItemImpl *This;

    TRACE("%s\n", debugstr_w(path));

    *item = NULL;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->FolderItem2_iface.lpVtbl = &FolderItemImpl_Vtbl;
    This->ref = 1;
    if (path)
        This->path = strdupW(path);

    This->folder = folder;
    Folder3_AddRef(&folder->Folder3_iface);

    if (SHParseDisplayName(This->path, NULL, &pidl, ~0u, &This->attributes) == S_OK)
        ILFree(pidl);

    *item = (FolderItem *)&This->FolderItem2_iface;
    return S_OK;
}

static HRESULT WINAPI FolderItemsImpl_QueryInterface(FolderItems3 *iface,
        REFIID riid, LPVOID *ppv)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);

    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IDispatch, riid) ||
        IsEqualIID(&IID_FolderItems, riid) ||
        IsEqualIID(&IID_FolderItems2, riid) ||
        IsEqualIID(&IID_FolderItems3, riid))
        *ppv = &This->FolderItems3_iface;
    else
    {
        WARN("not implemented for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI FolderItemsImpl_AddRef(FolderItems3 *iface)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    return ref;
}

static ULONG WINAPI FolderItemsImpl_Release(FolderItems3 *iface)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    LONG i;

    TRACE("(%p), new refcount=%i\n", iface, ref);

    if (!ref)
    {
        Folder3_Release(&This->folder->Folder3_iface);
        for (i = 0; i < This->item_count; i++)
            HeapFree(GetProcessHeap(), 0, This->item_names[i]);
        HeapFree(GetProcessHeap(), 0, This->item_names);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static HRESULT WINAPI FolderItemsImpl_GetTypeInfoCount(FolderItems3 *iface,
        UINT *count)
{
    TRACE("(%p,%p)\n", iface, count);

    *count = 1;
    return S_OK;
}

static HRESULT WINAPI FolderItemsImpl_GetTypeInfo(FolderItems3 *iface,
        UINT type, LCID lcid, ITypeInfo **ppti)
{
    HRESULT hr;

    TRACE("(%p,%u,%d,%p)\n", iface, type, lcid, ppti);

    hr = get_typeinfo(FolderItems3_tid, ppti);
    if (SUCCEEDED(hr))
        ITypeInfo_AddRef(*ppti);
    return hr;
}

static HRESULT WINAPI FolderItemsImpl_GetIDsOfNames(FolderItems3 *iface,
        REFIID riid, LPOLESTR *names, UINT count, LCID lcid, DISPID *dispid)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%s,%p,%u,%d,%p)\n", iface, shdebugstr_guid(riid), names, count, lcid, dispid);

    hr = get_typeinfo(FolderItems3_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_GetIDsOfNames(ti, names, count, dispid);
    return hr;
}

static HRESULT WINAPI FolderItemsImpl_Invoke(FolderItems3 *iface,
        DISPID dispid, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *result, EXCEPINFO *ei, UINT *err)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%d,%s,%d,%u,%p,%p,%p,%p)\n", iface, dispid, shdebugstr_guid(riid), lcid, flags, params, result, ei, err);

    hr = get_typeinfo(FolderItems3_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_Invoke(ti, This, dispid, flags, params, result, ei, err);
    return hr;
}

static HRESULT WINAPI FolderItemsImpl_get_Count(FolderItems3 *iface, LONG *count)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);

    TRACE("(%p,%p)\n", iface, count);

    *count = PathIsDirectoryW(V_BSTR(&This->folder->dir)) ? This->item_count : 0;
    return S_OK;
}

static HRESULT WINAPI FolderItemsImpl_get_Application(FolderItems3 *iface, IDispatch **disp)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);

    TRACE("(%p,%p)\n", iface, disp);

    return Folder3_get_Application(&This->folder->Folder3_iface, disp);
}

static HRESULT WINAPI FolderItemsImpl_get_Parent(FolderItems3 *iface, IDispatch **ppid)
{
    TRACE("(%p,%p)\n", iface, ppid);

    if (ppid)
        *ppid = NULL;

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemsImpl_Item(FolderItems3 *iface, VARIANT index, FolderItem **item)
{
    FolderItemsImpl *This = impl_from_FolderItems(iface);
    WCHAR buffW[MAX_PATH], *display_name;
    HRESULT hr;

    TRACE("(%p,%s,%p)\n", iface, debugstr_variant(&index), item);

    *item = NULL;

    if (!PathIsDirectoryW(V_BSTR(&This->folder->dir)))
        return S_FALSE;

    switch (V_VT(&index))
    {
        case VT_I2:
            VariantChangeType(&index, &index, 0, VT_I4);
            /* fall through */

        case VT_I4:
            if (V_I4(&index) >= This->item_count || V_I4(&index) < 0)
                return S_FALSE;

            display_name = This->item_names[V_I4(&index)];
            break;

        case VT_BSTR:
        {
            LPITEMIDLIST pidl;
            STRRET strret;

            if (!V_BSTR(&index))
                return S_FALSE;

            if (FAILED(hr = IShellFolder2_ParseDisplayName(This->folder->folder, NULL, NULL, V_BSTR(&index),
                    NULL, &pidl, NULL)))
                return S_FALSE;

            IShellFolder2_GetDisplayNameOf(This->folder->folder, pidl, SHGDN_FORPARSING, &strret);
            StrRetToBufW(&strret, NULL, buffW, sizeof(buffW)/sizeof(*buffW));
            ILFree(pidl);

            display_name = buffW;
            break;
        }
        case VT_ERROR:
            return FolderItem_Constructor(This->folder, NULL, item);

        default:
            FIXME("Index type %d not handled.\n", V_VT(&index));
            /* fall through */
        case VT_EMPTY:
            return E_NOTIMPL;
    }

    return FolderItem_Constructor(This->folder, display_name, item);
}

static HRESULT WINAPI FolderItemsImpl__NewEnum(FolderItems3 *iface, IUnknown **ppunk)
{
    FIXME("(%p,%p)\n", iface, ppunk);

    if (!ppunk)
        return E_INVALIDARG;

    *ppunk = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemsImpl_InvokeVerbEx(FolderItems3 *iface, VARIANT verb, VARIANT args)
{
    FIXME("(%p,%s,%s)\n", iface, debugstr_variant(&verb), debugstr_variant(&args));

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemsImpl_Filter(FolderItems3 *iface, LONG flags, BSTR spec)
{
    FIXME("(%p,%d,%s)\n", iface, flags, wine_dbgstr_w(spec));

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderItemsImpl_get_Verbs(FolderItems3 *iface, FolderItemVerbs **ppfic)
{
    FIXME("(%p,%p)\n", iface, ppfic);

    if (!ppfic)
        return E_INVALIDARG;

    *ppfic = NULL;
    return E_NOTIMPL;
}

static const FolderItems3Vtbl FolderItemsImpl_Vtbl = {
    FolderItemsImpl_QueryInterface,
    FolderItemsImpl_AddRef,
    FolderItemsImpl_Release,
    FolderItemsImpl_GetTypeInfoCount,
    FolderItemsImpl_GetTypeInfo,
    FolderItemsImpl_GetIDsOfNames,
    FolderItemsImpl_Invoke,
    FolderItemsImpl_get_Count,
    FolderItemsImpl_get_Application,
    FolderItemsImpl_get_Parent,
    FolderItemsImpl_Item,
    FolderItemsImpl__NewEnum,
    FolderItemsImpl_InvokeVerbEx,
    FolderItemsImpl_Filter,
    FolderItemsImpl_get_Verbs
};

static void idlist_sort(LPITEMIDLIST *idlist, unsigned int l, unsigned int r, IShellFolder2 *folder)
{
    unsigned int m;

    if (l == r)
        return;

    if (r < l)
    {
        idlist_sort(idlist, r, l, folder);
        return;
    }

    m = (l + r) / 2;
    idlist_sort(idlist, l, m, folder);
    idlist_sort(idlist, m + 1, r, folder);

    /* join the two sides */
    while (l <= m && m < r)
    {
        if ((short)IShellFolder2_CompareIDs(folder, 0, idlist[l], idlist[m + 1]) > 0)
        {
            LPITEMIDLIST t = idlist[m + 1];
            memmove(&idlist[l + 1], &idlist[l], (m - l + 1) * sizeof(idlist[l]));
            idlist[l] = t;

            m++;
        }
        l++;
    }
}

static HRESULT FolderItems_Constructor(FolderImpl *folder, FolderItems **ret)
{
    IEnumIDList *enumidlist;
    FolderItemsImpl *This;
    LPITEMIDLIST pidl;
    unsigned int i;
    HRESULT hr;

    TRACE("(%s,%p)\n", debugstr_variant(&folder->dir), ret);

    *ret = NULL;

    if (V_VT(&folder->dir) == VT_I4)
    {
        FIXME("special folder constants are not supported\n");
        return E_NOTIMPL;
    }

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->FolderItems3_iface.lpVtbl = &FolderItemsImpl_Vtbl;
    This->ref = 1;
    This->folder = folder;
    Folder3_AddRef(&folder->Folder3_iface);

    enumidlist = NULL;
    if (FAILED(hr = IShellFolder2_EnumObjects(folder->folder, NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
            &enumidlist)))
    {
        goto failed;
    }

    while (IEnumIDList_Next(enumidlist, 1, &pidl, NULL) == S_OK)
    {
        This->item_count++;
        ILFree(pidl);
    }

    if (This->item_count)
    {
        LPITEMIDLIST *pidls;
        ULONG fetched;

        pidls = HeapAlloc(GetProcessHeap(), 0, This->item_count * sizeof(*pidls));
        This->item_names = HeapAlloc(GetProcessHeap(), 0, This->item_count * sizeof(*This->item_names));

        if (!pidls || !This->item_names)
        {
            HeapFree(GetProcessHeap(), 0, pidls);
            HeapFree(GetProcessHeap(), 0, This->item_names);
            hr = E_OUTOFMEMORY;
            goto failed;
        }

        IEnumIDList_Reset(enumidlist);
        if (IEnumIDList_Next(enumidlist, This->item_count, pidls, &fetched) == S_OK)
            idlist_sort(pidls, 0, This->item_count - 1, folder->folder);

        for (i = 0; i < This->item_count; i++)
        {
            WCHAR buffW[MAX_PATH];
            STRRET strret;

            IShellFolder2_GetDisplayNameOf(folder->folder, pidls[i], SHGDN_FORPARSING, &strret);
            StrRetToBufW(&strret, NULL, buffW, sizeof(buffW)/sizeof(*buffW));

            This->item_names[i] = strdupW(buffW);

            ILFree(pidls[i]);
        }
        HeapFree(GetProcessHeap(), 0, pidls);
    }
    IEnumIDList_Release(enumidlist);

    *ret = (FolderItems *)&This->FolderItems3_iface;
    return S_OK;

failed:
    if (enumidlist)
        IEnumIDList_Release(enumidlist);
    return hr;
}

static HRESULT WINAPI FolderImpl_QueryInterface(Folder3 *iface, REFIID riid,
        LPVOID *ppv)
{
    FolderImpl *This = impl_from_Folder(iface);

    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IDispatch, riid) ||
        IsEqualIID(&IID_Folder, riid) ||
        IsEqualIID(&IID_Folder2, riid) ||
        IsEqualIID(&IID_Folder3, riid))
        *ppv = &This->Folder3_iface;
    else
    {
        WARN("not implemented for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI FolderImpl_AddRef(Folder3 *iface)
{
    FolderImpl *This = impl_from_Folder(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    return ref;
}

static ULONG WINAPI FolderImpl_Release(Folder3 *iface)
{
    FolderImpl *This = impl_from_Folder(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    if (!ref)
    {
        ILFree(This->pidl);
        IShellFolder2_Release(This->folder);
        IDispatch_Release(This->application);
        VariantClear(&This->dir);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static HRESULT WINAPI FolderImpl_GetTypeInfoCount(Folder3 *iface, UINT *pctinfo)
{
    TRACE("(%p,%p)\n", iface, pctinfo);

    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI FolderImpl_GetTypeInfo(Folder3 *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HRESULT hr;

    TRACE("(%p,%u,%d,%p)\n", iface, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(Folder3_tid, ppTInfo);
    if (SUCCEEDED(hr))
        ITypeInfo_AddRef(*ppTInfo);

    return hr;
}

static HRESULT WINAPI FolderImpl_GetIDsOfNames(Folder3 *iface, REFIID riid,
        LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%s,%p,%u,%d,%p)\n", iface, shdebugstr_guid(riid), rgszNames, cNames, lcid,
            rgDispId);

    hr = get_typeinfo(Folder3_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_GetIDsOfNames(ti, rgszNames, cNames, rgDispId);
    return hr;
}

static HRESULT WINAPI FolderImpl_Invoke(Folder3 *iface, DISPID dispIdMember,
        REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    FolderImpl *This = impl_from_Folder(iface);
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%d,%s,%d,%u,%p,%p,%p,%p)\n", iface, dispIdMember, shdebugstr_guid(riid), lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(Folder3_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_Invoke(ti, This, dispIdMember, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
    return hr;
}

static HRESULT WINAPI FolderImpl_get_Title(Folder3 *iface, BSTR *title)
{
    FolderImpl *This = impl_from_Folder(iface);
    PCUITEMID_CHILD last_part;
    IShellFolder2 *parent;
    WCHAR buffW[MAX_PATH];
    SHELLDETAILS sd;
    HRESULT hr;

    TRACE("(%p,%p)\n", iface, title);

    *title = NULL;

    if (FAILED(hr = SHBindToParent(This->pidl, &IID_IShellFolder2, (void **)&parent, &last_part)))
        return hr;

    hr = IShellFolder2_GetDetailsOf(parent, last_part, 0, &sd);
    IShellFolder2_Release(parent);
    if (FAILED(hr))
        return hr;

    StrRetToBufW(&sd.str, NULL, buffW, sizeof(buffW)/sizeof(buffW[0]));
    *title = SysAllocString(buffW);

    return *title ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI FolderImpl_get_Application(Folder3 *iface, IDispatch **disp)
{
    FolderImpl *This = impl_from_Folder(iface);

    TRACE("(%p,%p)\n", iface, disp);

    if (!disp)
        return E_INVALIDARG;

    *disp = This->application;
    IDispatch_AddRef(*disp);

    return S_OK;
}

static HRESULT WINAPI FolderImpl_get_Parent(Folder3 *iface, IDispatch **disp)
{
    TRACE("(%p,%p)\n", iface, disp);

    if (disp)
        *disp = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_get_ParentFolder(Folder3 *iface, Folder **ppsf)
{
    FIXME("(%p,%p)\n", iface, ppsf);

    *ppsf = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_Items(Folder3 *iface, FolderItems **ppid)
{
    FolderImpl *This = impl_from_Folder(iface);

    TRACE("(%p,%p)\n", iface, ppid);

    return FolderItems_Constructor(This, ppid);
}

static HRESULT WINAPI FolderImpl_ParseName(Folder3 *iface, BSTR name, FolderItem **item)
{
    FolderImpl *This = impl_from_Folder(iface);
    WCHAR pathW[MAX_PATH];
    LPITEMIDLIST pidl;
    STRRET strret;
    HRESULT hr;

    TRACE("(%p,%s,%p)\n", iface, debugstr_w(name), item);

    *item = NULL;

    if (FAILED(IShellFolder2_ParseDisplayName(This->folder, NULL, NULL, name, NULL, &pidl, NULL)))
        return S_FALSE;

    hr = IShellFolder2_GetDisplayNameOf(This->folder, pidl, SHGDN_FORPARSING, &strret);
    ILFree(pidl);
    if (FAILED(hr))
        return S_FALSE;

    StrRetToBufW(&strret, NULL, pathW, sizeof(pathW)/sizeof(*pathW));

    return FolderItem_Constructor(This, pathW, item);
}

static HRESULT WINAPI FolderImpl_NewFolder(Folder3 *iface, BSTR bName,
        VARIANT vOptions)
{
    FIXME("(%p,%s)\n", iface, debugstr_w(bName));

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_MoveHere(Folder3 *iface, VARIANT vItem,
        VARIANT vOptions)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_CopyHere(Folder3 *iface, VARIANT vItem,
        VARIANT vOptions)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_GetDetailsOf(Folder3 *iface, VARIANT vItem,
        int iColumn, BSTR *pbs)
{
    FIXME("(%p,%d,%p)\n", iface, iColumn, pbs);

    *pbs = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_get_Self(Folder3 *iface, FolderItem **item)
{
    FolderImpl *This = impl_from_Folder(iface);

    TRACE("(%p,%p)\n", iface, item);

    return FolderItem_Constructor(This, NULL, item);
}

static HRESULT WINAPI FolderImpl_get_OfflineStatus(Folder3 *iface, LONG *pul)
{
    FIXME("(%p,%p)\n", iface, pul);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_Synchronize(Folder3 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_get_HaveToShowWebViewBarricade(Folder3 *iface,
        VARIANT_BOOL *pbHaveToShowWebViewBarricade)
{
    FIXME("(%p,%p)\n", iface, pbHaveToShowWebViewBarricade);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_DismissedWebViewBarricade(Folder3 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_get_ShowWebViewBarricade(Folder3 *iface,
        VARIANT_BOOL *pbShowWebViewBarricade)
{
    FIXME("(%p,%p)\n", iface, pbShowWebViewBarricade);

    return E_NOTIMPL;
}

static HRESULT WINAPI FolderImpl_put_ShowWebViewBarricade(Folder3 *iface,
        VARIANT_BOOL bShowWebViewBarricade)
{
    FIXME("(%p,%d)\n", iface, bShowWebViewBarricade);

    return E_NOTIMPL;
}

static const Folder3Vtbl FolderImpl_Vtbl = {
    FolderImpl_QueryInterface,
    FolderImpl_AddRef,
    FolderImpl_Release,
    FolderImpl_GetTypeInfoCount,
    FolderImpl_GetTypeInfo,
    FolderImpl_GetIDsOfNames,
    FolderImpl_Invoke,
    FolderImpl_get_Title,
    FolderImpl_get_Application,
    FolderImpl_get_Parent,
    FolderImpl_get_ParentFolder,
    FolderImpl_Items,
    FolderImpl_ParseName,
    FolderImpl_NewFolder,
    FolderImpl_MoveHere,
    FolderImpl_CopyHere,
    FolderImpl_GetDetailsOf,
    FolderImpl_get_Self,
    FolderImpl_get_OfflineStatus,
    FolderImpl_Synchronize,
    FolderImpl_get_HaveToShowWebViewBarricade,
    FolderImpl_DismissedWebViewBarricade,
    FolderImpl_get_ShowWebViewBarricade,
    FolderImpl_put_ShowWebViewBarricade
};

static HRESULT Folder_Constructor(VARIANT *dir, IShellFolder2 *folder, LPITEMIDLIST pidl, Folder **ret)
{
    PCUITEMID_CHILD last_part;
    IShellFolder2 *parent;
    FolderImpl *This;
    STRRET strret;
    HRESULT hr;

    *ret = NULL;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->Folder3_iface.lpVtbl = &FolderImpl_Vtbl;
    This->ref = 1;
    This->folder = folder;
    This->pidl = pidl;

    hr = SHBindToParent(pidl, &IID_IShellFolder2, (void **)&parent, &last_part);
    IShellFolder2_GetDisplayNameOf(parent, last_part, SHGDN_FORPARSING, &strret);
    StrRetToBufW(&strret, NULL, This->path, sizeof(This->path)/sizeof(*This->path));
    IShellFolder2_Release(parent);

    IShellDispatch_Constructor(NULL, &IID_IDispatch, (void **)&This->application);

    VariantInit(&This->dir);
    hr = VariantCopy(&This->dir, dir);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, This);
        return E_OUTOFMEMORY;
    }

    *ret = (Folder *)&This->Folder3_iface;
    return hr;
}

static HRESULT WINAPI ShellDispatch_QueryInterface(IShellDispatch6 *iface,
        REFIID riid, LPVOID *ppv)
{
    ShellDispatch *This = impl_from_IShellDispatch6(iface);

    TRACE("(%p,%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv) return E_INVALIDARG;

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IDispatch, riid) ||
        IsEqualIID(&IID_IShellDispatch, riid) ||
        IsEqualIID(&IID_IShellDispatch2, riid) ||
        IsEqualIID(&IID_IShellDispatch3, riid) ||
        IsEqualIID(&IID_IShellDispatch4, riid) ||
        IsEqualIID(&IID_IShellDispatch5, riid) ||
        IsEqualIID(&IID_IShellDispatch6, riid))
        *ppv = &This->IShellDispatch6_iface;
    else
    {
        WARN("not implemented for %s\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IShellDispatch6_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI ShellDispatch_AddRef(IShellDispatch6 *iface)
{
    ShellDispatch *This = impl_from_IShellDispatch6(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    return ref;
}

static ULONG WINAPI ShellDispatch_Release(IShellDispatch6 *iface)
{
    ShellDispatch *This = impl_from_IShellDispatch6(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p), new refcount=%i\n", iface, ref);

    if (!ref)
        HeapFree(GetProcessHeap(), 0, This);

    return ref;
}

static HRESULT WINAPI ShellDispatch_GetTypeInfoCount(IShellDispatch6 *iface,
        UINT *pctinfo)
{
    TRACE("(%p,%p)\n", iface, pctinfo);

    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI ShellDispatch_GetTypeInfo(IShellDispatch6 *iface,
        UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
{
    HRESULT hr;

    TRACE("(%p,%u,%d,%p)\n", iface, iTInfo, lcid, ppTInfo);

    hr = get_typeinfo(IShellDispatch6_tid, ppTInfo);
    if (SUCCEEDED(hr))
        ITypeInfo_AddRef(*ppTInfo);
    return hr;
}

static HRESULT WINAPI ShellDispatch_GetIDsOfNames(IShellDispatch6 *iface,
        REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%s,%p,%u,%d,%p)\n", iface, shdebugstr_guid(riid), rgszNames, cNames, lcid,
            rgDispId);

    hr = get_typeinfo(IShellDispatch6_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_GetIDsOfNames(ti, rgszNames, cNames, rgDispId);
    return hr;
}

static HRESULT WINAPI ShellDispatch_Invoke(IShellDispatch6 *iface,
        DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
        DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo,
        UINT *puArgErr)
{
    ShellDispatch *This = impl_from_IShellDispatch6(iface);
    ITypeInfo *ti;
    HRESULT hr;

    TRACE("(%p,%d,%s,%d,%u,%p,%p,%p,%p)\n", iface, dispIdMember, shdebugstr_guid(riid), lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    hr = get_typeinfo(IShellDispatch6_tid, &ti);
    if (SUCCEEDED(hr))
        hr = ITypeInfo_Invoke(ti, &This->IShellDispatch6_iface, dispIdMember, wFlags, pDispParams,
            pVarResult, pExcepInfo, puArgErr);
    return hr;
}

static HRESULT WINAPI ShellDispatch_get_Application(IShellDispatch6 *iface, IDispatch **disp)
{
    TRACE("(%p,%p)\n", iface, disp);

    if (!disp)
        return E_INVALIDARG;

    *disp = (IDispatch *)iface;
    IDispatch_AddRef(*disp);
    return S_OK;
}

static HRESULT WINAPI ShellDispatch_get_Parent(IShellDispatch6 *iface, IDispatch **disp)
{
    TRACE("(%p,%p)\n", iface, disp);

    if (disp)
    {
        *disp = (IDispatch *)iface;
        IDispatch_AddRef(*disp);
    }

    return S_OK;
}

static HRESULT WINAPI ShellDispatch_NameSpace(IShellDispatch6 *iface,
        VARIANT dir, Folder **ret)
{
    IShellFolder2 *folder;
    IShellFolder *desktop;
    LPITEMIDLIST pidl;
    HRESULT hr;

    TRACE("(%p,%s,%p)\n", iface, debugstr_variant(&dir), ret);

    *ret = NULL;

    switch (V_VT(&dir))
    {
        case VT_I2:
            if (FAILED(hr = VariantChangeType(&dir, &dir, 0, VT_I4)))
                return hr;

            /* fallthrough */
        case VT_I4:
            if (FAILED(hr = SHGetFolderLocation(NULL, V_I4(&dir), NULL, 0, &pidl)))
                return S_FALSE;

            break;
        case VT_BSTR:
            if (FAILED(hr = SHParseDisplayName(V_BSTR(&dir), NULL, &pidl, 0, NULL)))
                return S_FALSE;

            break;
        default:
            WARN("Ignoring directory value %s\n", debugstr_variant(&dir));
            return S_FALSE;
    }

    if (FAILED(hr = SHGetDesktopFolder(&desktop)))
        return hr;

    if (_ILIsDesktop(pidl))
        hr = IShellFolder_QueryInterface(desktop, &IID_IShellFolder2, (void **)&folder);
    else
        hr = IShellFolder_BindToObject(desktop, pidl, NULL, &IID_IShellFolder2, (void **)&folder);

    IShellFolder_Release(desktop);

    if (FAILED(hr))
        return S_FALSE;

    return Folder_Constructor(&dir, folder, pidl, ret);
}

static HRESULT WINAPI ShellDispatch_BrowseForFolder(IShellDispatch6 *iface,
        LONG Hwnd, BSTR Title, LONG Options, VARIANT RootFolder, Folder **ppsdf)
{
    FIXME("(%p,%x,%s,%x,%p)\n", iface, Hwnd, debugstr_w(Title), Options, ppsdf);

    *ppsdf = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_Windows(IShellDispatch6 *iface,
        IDispatch **ppid)
{
    FIXME("(%p,%p)\n", iface, ppid);

    *ppid = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_Open(IShellDispatch6 *iface, VARIANT vDir)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_Explore(IShellDispatch6 *iface, VARIANT vDir)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_MinimizeAll(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_UndoMinimizeALL(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_FileRun(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_CascadeWindows(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_TileVertically(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_TileHorizontally(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ShutdownWindows(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_Suspend(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_EjectPC(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_SetTime(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_TrayProperties(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_Help(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_FindFiles(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_FindComputer(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_RefreshMenu(IShellDispatch6 *iface)
{
    FIXME("(%p)\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ControlPanelItem(IShellDispatch6 *iface,
        BSTR szDir)
{
    FIXME("(%p,%s)\n", iface, debugstr_w(szDir));

    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_IsRestricted(IShellDispatch6 *iface, BSTR group, BSTR restriction, LONG *value)
{
    FIXME("(%s, %s, %p): stub\n", debugstr_w(group), debugstr_w(restriction), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ShellExecute(IShellDispatch6 *iface,
        BSTR file, VARIANT v_args, VARIANT v_dir, VARIANT v_op, VARIANT v_show)
{
    VARIANT args_str, dir_str, op_str, show_int;
    WCHAR *args = NULL, *dir = NULL, *op = NULL;
    INT show = 0;
    HINSTANCE ret;

    TRACE("(%s, %s, %s, %s, %s)\n", debugstr_w(file), debugstr_variant(&v_args),
            debugstr_variant(&v_dir), debugstr_variant(&v_op), debugstr_variant(&v_show));

    VariantInit(&args_str);
    VariantChangeType(&args_str, &v_args, 0, VT_BSTR);
    if (V_VT(&args_str) == VT_BSTR)
        args = V_BSTR(&args_str);

    VariantInit(&dir_str);
    VariantChangeType(&dir_str, &v_dir, 0, VT_BSTR);
    if (V_VT(&dir_str) == VT_BSTR)
        dir = V_BSTR(&dir_str);

    VariantInit(&op_str);
    VariantChangeType(&op_str, &v_op, 0, VT_BSTR);
    if (V_VT(&op_str) == VT_BSTR)
        op = V_BSTR(&op_str);

    VariantInit(&show_int);
    VariantChangeType(&show_int, &v_show, 0, VT_I4);
    if (V_VT(&show_int) == VT_I4)
        show = V_I4(&show_int);

    ret = ShellExecuteW(NULL, op, file, args, dir, show);

    VariantClear(&args_str);
    VariantClear(&dir_str);
    VariantClear(&op_str);
    VariantClear(&show_int);

    return (ULONG_PTR)ret > 32 ? S_OK : S_FALSE;
}

static HRESULT WINAPI ShellDispatch_FindPrinter(IShellDispatch6 *iface, BSTR name, BSTR location, BSTR model)
{
    FIXME("(%s, %s, %s): stub\n", debugstr_w(name), debugstr_w(location), debugstr_w(model));
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_GetSystemInformation(IShellDispatch6 *iface, BSTR name, VARIANT *ret)
{
    FIXME("(%s, %p): stub\n", debugstr_w(name), ret);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ServiceStart(IShellDispatch6 *iface, BSTR service, VARIANT persistent, VARIANT *ret)
{
    FIXME("(%s, %p): stub\n", debugstr_w(service), ret);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ServiceStop(IShellDispatch6 *iface, BSTR service, VARIANT persistent, VARIANT *ret)
{
    FIXME("(%s, %p): stub\n", debugstr_w(service), ret);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_IsServiceRunning(IShellDispatch6 *iface, BSTR name, VARIANT *running)
{
    SERVICE_STATUS_PROCESS status;
    SC_HANDLE scm, service;
    DWORD dummy;

    TRACE("(%s, %p)\n", debugstr_w(name), running);

    V_VT(running) = VT_BOOL;
    V_BOOL(running) = VARIANT_FALSE;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm)
    {
        ERR("failed to connect to service manager\n");
        return S_OK;
    }

    service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (!service)
    {
        ERR("Failed to open service %s (%u)\n", debugstr_w(name), GetLastError());
        CloseServiceHandle(scm);
        return S_OK;
    }

    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&status,
             sizeof(SERVICE_STATUS_PROCESS), &dummy))
    {
        TRACE("failed to query service status (%u)\n", GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return S_OK;
    }

    if (status.dwCurrentState == SERVICE_RUNNING)
       V_BOOL(running) = VARIANT_TRUE;

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    return S_OK;
}

static HRESULT WINAPI ShellDispatch_CanStartStopService(IShellDispatch6 *iface, BSTR service, VARIANT *ret)
{
    FIXME("(%s, %p): stub\n", debugstr_w(service), ret);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ShowBrowserBar(IShellDispatch6 *iface, BSTR clsid, VARIANT show, VARIANT *ret)
{
    FIXME("(%s, %p): stub\n", debugstr_w(clsid), ret);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_AddToRecent(IShellDispatch6 *iface, VARIANT file, BSTR category)
{
    FIXME("(%s): stub\n", debugstr_w(category));
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_WindowsSecurity(IShellDispatch6 *iface)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ToggleDesktop(IShellDispatch6 *iface)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_ExplorerPolicy(IShellDispatch6 *iface, BSTR policy, VARIANT *value)
{
    FIXME("(%s, %p): stub\n", debugstr_w(policy), value);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_GetSetting(IShellDispatch6 *iface, LONG setting, VARIANT_BOOL *result)
{
    FIXME("(%d %p): stub\n", setting, result);
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_WindowSwitcher(IShellDispatch6 *iface)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI ShellDispatch_SearchCommand(IShellDispatch6 *iface)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static const IShellDispatch6Vtbl ShellDispatchVtbl = {
    ShellDispatch_QueryInterface,
    ShellDispatch_AddRef,
    ShellDispatch_Release,
    ShellDispatch_GetTypeInfoCount,
    ShellDispatch_GetTypeInfo,
    ShellDispatch_GetIDsOfNames,
    ShellDispatch_Invoke,
    ShellDispatch_get_Application,
    ShellDispatch_get_Parent,
    ShellDispatch_NameSpace,
    ShellDispatch_BrowseForFolder,
    ShellDispatch_Windows,
    ShellDispatch_Open,
    ShellDispatch_Explore,
    ShellDispatch_MinimizeAll,
    ShellDispatch_UndoMinimizeALL,
    ShellDispatch_FileRun,
    ShellDispatch_CascadeWindows,
    ShellDispatch_TileVertically,
    ShellDispatch_TileHorizontally,
    ShellDispatch_ShutdownWindows,
    ShellDispatch_Suspend,
    ShellDispatch_EjectPC,
    ShellDispatch_SetTime,
    ShellDispatch_TrayProperties,
    ShellDispatch_Help,
    ShellDispatch_FindFiles,
    ShellDispatch_FindComputer,
    ShellDispatch_RefreshMenu,
    ShellDispatch_ControlPanelItem,
    ShellDispatch_IsRestricted,
    ShellDispatch_ShellExecute,
    ShellDispatch_FindPrinter,
    ShellDispatch_GetSystemInformation,
    ShellDispatch_ServiceStart,
    ShellDispatch_ServiceStop,
    ShellDispatch_IsServiceRunning,
    ShellDispatch_CanStartStopService,
    ShellDispatch_ShowBrowserBar,
    ShellDispatch_AddToRecent,
    ShellDispatch_WindowsSecurity,
    ShellDispatch_ToggleDesktop,
    ShellDispatch_ExplorerPolicy,
    ShellDispatch_GetSetting,
    ShellDispatch_WindowSwitcher,
    ShellDispatch_SearchCommand
};

HRESULT WINAPI IShellDispatch_Constructor(IUnknown *outer, REFIID riid, void **ppv)
{
    ShellDispatch *This;
    HRESULT ret;

    TRACE("(%p, %s)\n", outer, debugstr_guid(riid));

    *ppv = NULL;

    if (outer) return CLASS_E_NOAGGREGATION;

    This = HeapAlloc(GetProcessHeap(), 0, sizeof(ShellDispatch));
    if (!This) return E_OUTOFMEMORY;
    This->IShellDispatch6_iface.lpVtbl = &ShellDispatchVtbl;
    This->ref = 1;

    ret = IShellDispatch6_QueryInterface(&This->IShellDispatch6_iface, riid, ppv);
    IShellDispatch6_Release(&This->IShellDispatch6_iface);
    return ret;
}
