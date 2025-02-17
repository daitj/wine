/*
 * Copyright 2006 Jacek Caban for CodeWeavers
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

#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "wine/debug.h"

#include "mshtml_private.h"
#include "htmlevent.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

static HTMLDOMNode *get_node_obj(IHTMLDOMNode*);
static HRESULT create_node(HTMLDocumentNode*,nsIDOMNode*,HTMLDOMNode**);

static ExternalCycleCollectionParticipant node_ccp;

typedef struct {
    DispatchEx dispex;
    IHTMLDOMChildrenCollection IHTMLDOMChildrenCollection_iface;

    LONG ref;

    nsIDOMNodeList *nslist;
} HTMLDOMChildrenCollection;

typedef struct {
    IEnumVARIANT IEnumVARIANT_iface;

    LONG ref;

    ULONG iter;
    HTMLDOMChildrenCollection *col;
} HTMLDOMChildrenCollectionEnum;

static inline HTMLDOMChildrenCollectionEnum *impl_from_IEnumVARIANT(IEnumVARIANT *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMChildrenCollectionEnum, IEnumVARIANT_iface);
}

static HRESULT WINAPI HTMLDOMChildrenCollectionEnum_QueryInterface(IEnumVARIANT *iface, REFIID riid, void **ppv)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(riid, &IID_IUnknown)) {
        *ppv = &This->IEnumVARIANT_iface;
    }else if(IsEqualGUID(riid, &IID_IEnumVARIANT)) {
        *ppv = &This->IEnumVARIANT_iface;
    }else {
        FIXME("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI HTMLDOMChildrenCollectionEnum_AddRef(IEnumVARIANT *iface)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLDOMChildrenCollectionEnum_Release(IEnumVARIANT *iface)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        IHTMLDOMChildrenCollection_Release(&This->col->IHTMLDOMChildrenCollection_iface);
        heap_free(This);
    }

    return ref;
}

static ULONG get_enum_len(HTMLDOMChildrenCollectionEnum *This)
{
    UINT32 len;
    nsresult nsres;

    nsres = nsIDOMNodeList_GetLength(This->col->nslist, &len);
    assert(nsres == NS_OK);
    return len;
}

static HRESULT WINAPI HTMLDOMChildrenCollectionEnum_Next(IEnumVARIANT *iface, ULONG celt, VARIANT *rgVar, ULONG *pCeltFetched)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);
    ULONG fetched = 0, len;
    nsIDOMNode *nsnode;
    HTMLDOMNode *node;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%ld %p %p)\n", This, celt, rgVar, pCeltFetched);

    len = get_enum_len(This);

    while(This->iter+fetched < len && fetched < celt) {
        nsres = nsIDOMNodeList_Item(This->col->nslist, This->iter+fetched, &nsnode);
        assert(nsres == NS_OK);

        hres = get_node(nsnode, TRUE, &node);
        nsIDOMNode_Release(nsnode);
        if(FAILED(hres)) {
            ERR("get_node failed: %08lx\n", hres);
            break;
        }

        V_VT(rgVar+fetched) = VT_DISPATCH;
        IHTMLDOMNode_AddRef(&node->IHTMLDOMNode_iface);
        V_DISPATCH(rgVar+fetched) = (IDispatch*)&node->IHTMLDOMNode_iface;
        fetched++;
    }

    This->iter += fetched;
    if(pCeltFetched)
        *pCeltFetched = fetched;
    return fetched == celt ? S_OK : S_FALSE;
}

static HRESULT WINAPI HTMLDOMChildrenCollectionEnum_Skip(IEnumVARIANT *iface, ULONG celt)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);
    ULONG len;

    TRACE("(%p)->(%ld)\n", This, celt);

    len = get_enum_len(This);
    if(This->iter + celt > len) {
        This->iter = len;
        return S_FALSE;
    }

    This->iter += celt;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMChildrenCollectionEnum_Reset(IEnumVARIANT *iface)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);

    TRACE("(%p)->()\n", This);

    This->iter = 0;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMChildrenCollectionEnum_Clone(IEnumVARIANT *iface, IEnumVARIANT **ppEnum)
{
    HTMLDOMChildrenCollectionEnum *This = impl_from_IEnumVARIANT(iface);
    FIXME("(%p)->(%p)\n", This, ppEnum);
    return E_NOTIMPL;
}

static const IEnumVARIANTVtbl HTMLDOMChildrenCollectionEnumVtbl = {
    HTMLDOMChildrenCollectionEnum_QueryInterface,
    HTMLDOMChildrenCollectionEnum_AddRef,
    HTMLDOMChildrenCollectionEnum_Release,
    HTMLDOMChildrenCollectionEnum_Next,
    HTMLDOMChildrenCollectionEnum_Skip,
    HTMLDOMChildrenCollectionEnum_Reset,
    HTMLDOMChildrenCollectionEnum_Clone
};

static inline HTMLDOMChildrenCollection *impl_from_IHTMLDOMChildrenCollection(IHTMLDOMChildrenCollection *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMChildrenCollection, IHTMLDOMChildrenCollection_iface);
}

static HRESULT WINAPI HTMLDOMChildrenCollection_QueryInterface(IHTMLDOMChildrenCollection *iface, REFIID riid, void **ppv)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        *ppv = &This->IHTMLDOMChildrenCollection_iface;
    }else if(IsEqualGUID(&IID_IHTMLDOMChildrenCollection, riid)) {
        *ppv = &This->IHTMLDOMChildrenCollection_iface;
    }else if(dispex_query_interface(&This->dispex, riid, ppv)) {
        return *ppv ? S_OK : E_NOINTERFACE;
    }else {
        *ppv = NULL;
        WARN("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI HTMLDOMChildrenCollection_AddRef(IHTMLDOMChildrenCollection *iface)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    LONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLDOMChildrenCollection_Release(IHTMLDOMChildrenCollection *iface)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    LONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%ld\n", This, ref);

    if(!ref) {
        nsIDOMNodeList_Release(This->nslist);
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI HTMLDOMChildrenCollection_GetTypeInfoCount(IHTMLDOMChildrenCollection *iface, UINT *pctinfo)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    return IDispatchEx_GetTypeInfoCount(&This->dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLDOMChildrenCollection_GetTypeInfo(IHTMLDOMChildrenCollection *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    return IDispatchEx_GetTypeInfo(&This->dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLDOMChildrenCollection_GetIDsOfNames(IHTMLDOMChildrenCollection *iface, REFIID riid,
        LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    return IDispatchEx_GetIDsOfNames(&This->dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLDOMChildrenCollection_Invoke(IHTMLDOMChildrenCollection *iface, DISPID dispIdMember,
        REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    return IDispatchEx_Invoke(&This->dispex.IDispatchEx_iface, dispIdMember, riid, lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLDOMChildrenCollection_get_length(IHTMLDOMChildrenCollection *iface, LONG *p)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    UINT32 length=0;

    TRACE("(%p)->(%p)\n", This, p);

    nsIDOMNodeList_GetLength(This->nslist, &length);
    *p = length;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMChildrenCollection_get__newEnum(IHTMLDOMChildrenCollection *iface, IUnknown **p)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    HTMLDOMChildrenCollectionEnum *ret;

    TRACE("(%p)->(%p)\n", This, p);

    ret = heap_alloc(sizeof(*ret));
    if(!ret)
        return E_OUTOFMEMORY;

    ret->IEnumVARIANT_iface.lpVtbl = &HTMLDOMChildrenCollectionEnumVtbl;
    ret->ref = 1;
    ret->iter = 0;

    IHTMLDOMChildrenCollection_AddRef(&This->IHTMLDOMChildrenCollection_iface);
    ret->col = This;

    *p = (IUnknown*)&ret->IEnumVARIANT_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMChildrenCollection_item(IHTMLDOMChildrenCollection *iface, LONG index, IDispatch **ppItem)
{
    HTMLDOMChildrenCollection *This = impl_from_IHTMLDOMChildrenCollection(iface);
    nsIDOMNode *nsnode = NULL;
    HTMLDOMNode *node;
    UINT32 length=0;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%ld %p)\n", This, index, ppItem);

    if (ppItem)
        *ppItem = NULL;
    else
        return E_POINTER;

    nsIDOMNodeList_GetLength(This->nslist, &length);
    if(index < 0 || index >= length)
        return E_INVALIDARG;

    nsres = nsIDOMNodeList_Item(This->nslist, index, &nsnode);
    if(NS_FAILED(nsres) || !nsnode) {
        ERR("Item failed: %08lx\n", nsres);
        return E_FAIL;
    }

    hres = get_node(nsnode, TRUE, &node);
    if(FAILED(hres))
        return hres;

    *ppItem = (IDispatch*)&node->IHTMLDOMNode_iface;
    return S_OK;
}

static const IHTMLDOMChildrenCollectionVtbl HTMLDOMChildrenCollectionVtbl = {
    HTMLDOMChildrenCollection_QueryInterface,
    HTMLDOMChildrenCollection_AddRef,
    HTMLDOMChildrenCollection_Release,
    HTMLDOMChildrenCollection_GetTypeInfoCount,
    HTMLDOMChildrenCollection_GetTypeInfo,
    HTMLDOMChildrenCollection_GetIDsOfNames,
    HTMLDOMChildrenCollection_Invoke,
    HTMLDOMChildrenCollection_get_length,
    HTMLDOMChildrenCollection_get__newEnum,
    HTMLDOMChildrenCollection_item
};

static inline HTMLDOMChildrenCollection *impl_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMChildrenCollection, dispex);
}

#define DISPID_CHILDCOL_0 MSHTML_DISPID_CUSTOM_MIN

static HRESULT HTMLDOMChildrenCollection_get_dispid(DispatchEx *dispex, BSTR name, DWORD flags, DISPID *dispid)
{
    HTMLDOMChildrenCollection *This = impl_from_DispatchEx(dispex);
    WCHAR *ptr;
    DWORD idx=0;
    UINT32 len = 0;

    for(ptr = name; *ptr && is_digit(*ptr); ptr++)
        idx = idx*10 + (*ptr-'0');
    if(*ptr)
        return DISP_E_UNKNOWNNAME;

    nsIDOMNodeList_GetLength(This->nslist, &len);
    if(idx >= len)
        return DISP_E_UNKNOWNNAME;

    *dispid = DISPID_CHILDCOL_0 + idx;
    TRACE("ret %lx\n", *dispid);
    return S_OK;
}

static HRESULT HTMLDOMChildrenCollection_invoke(DispatchEx *dispex, IDispatch *this_obj, DISPID id, LCID lcid, WORD flags,
        DISPPARAMS *params, VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    HTMLDOMChildrenCollection *This = impl_from_DispatchEx(dispex);

    TRACE("(%p)->(%lx %lx %x %p %p %p %p)\n", This, id, lcid, flags, params, res, ei, caller);

    switch(flags) {
    case DISPATCH_PROPERTYGET: {
        IDispatch *disp = NULL;
        HRESULT hres;

        hres = IHTMLDOMChildrenCollection_item(&This->IHTMLDOMChildrenCollection_iface,
                id - DISPID_CHILDCOL_0, &disp);
        if(FAILED(hres))
            return hres;

        V_VT(res) = VT_DISPATCH;
        V_DISPATCH(res) = disp;
        break;
    }

    default:
        FIXME("unimplemented flags %x\n", flags);
        return E_NOTIMPL;
    }

    return S_OK;
}

static const dispex_static_data_vtbl_t HTMLDOMChildrenCollection_dispex_vtbl = {
    NULL,
    HTMLDOMChildrenCollection_get_dispid,
    HTMLDOMChildrenCollection_invoke,
    NULL
};

static const tid_t HTMLDOMChildrenCollection_iface_tids[] = {
    IHTMLDOMChildrenCollection_tid,
    0
};

dispex_static_data_t HTMLDOMChildrenCollection_dispex = {
    L"NodeList",
    &HTMLDOMChildrenCollection_dispex_vtbl,
    PROTO_ID_HTMLDOMChildrenCollection,
    DispDOMChildrenCollection_tid,
    HTMLDOMChildrenCollection_iface_tids,
    HTMLDOMNode_init_dispex_info
};

HRESULT create_child_collection(nsIDOMNodeList *nslist, HTMLDocumentNode *doc, IHTMLDOMChildrenCollection **ret)
{
    compat_mode_t compat_mode = doc ? dispex_compat_mode(&doc->node.event_target.dispex) : COMPAT_MODE_NONE;
    HTMLDOMChildrenCollection *collection;

    if(!(collection = heap_alloc_zero(sizeof(*collection))))
        return E_OUTOFMEMORY;

    collection->IHTMLDOMChildrenCollection_iface.lpVtbl = &HTMLDOMChildrenCollectionVtbl;
    collection->ref = 1;

    nsIDOMNodeList_AddRef(nslist);
    collection->nslist = nslist;

    init_dispatch(&collection->dispex, (IUnknown*)&collection->IHTMLDOMChildrenCollection_iface,
                  &HTMLDOMChildrenCollection_dispex, doc, compat_mode);

    *ret = &collection->IHTMLDOMChildrenCollection_iface;
    return S_OK;
}

static inline HTMLDOMNode *impl_from_IHTMLDOMNode(IHTMLDOMNode *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMNode, IHTMLDOMNode_iface);
}

static HRESULT WINAPI HTMLDOMNode_QueryInterface(IHTMLDOMNode *iface,
                                                 REFIID riid, void **ppv)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);

    return This->vtbl->qi(This, riid, ppv);
}

static ULONG WINAPI HTMLDOMNode_AddRef(IHTMLDOMNode *iface)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    LONG ref;

    ref = ccref_incr(&This->ccref, (nsISupports*)&This->IHTMLDOMNode_iface);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static ULONG WINAPI HTMLDOMNode_Release(IHTMLDOMNode *iface)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    LONG ref = ccref_decr(&This->ccref, (nsISupports*)&This->IHTMLDOMNode_iface, /*&node_ccp*/ NULL);

    TRACE("(%p) ref=%ld\n", This, ref);

    return ref;
}

static HRESULT WINAPI HTMLDOMNode_GetTypeInfoCount(IHTMLDOMNode *iface, UINT *pctinfo)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLDOMNode_GetTypeInfo(IHTMLDOMNode *iface, UINT iTInfo,
                                              LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLDOMNode_GetIDsOfNames(IHTMLDOMNode *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLDOMNode_Invoke(IHTMLDOMNode *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLDOMNode_get_nodeType(IHTMLDOMNode *iface, LONG *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    UINT16 type = -1;

    TRACE("(%p)->(%p)\n", This, p);

    nsIDOMNode_GetNodeType(This->nsnode, &type);

    switch(type) {
    case ELEMENT_NODE:
        *p = 1;
        break;
    case TEXT_NODE:
        *p = 3;
        break;
    case COMMENT_NODE:
    case DOCUMENT_TYPE_NODE:
        *p = 8;
        break;
    case DOCUMENT_NODE:
        *p = 9;
        break;
    case DOCUMENT_FRAGMENT_NODE:
        *p = 11;
        break;
    default:
        /*
         * FIXME:
         * According to MSDN only ELEMENT_NODE and TEXT_NODE are supported.
         * It needs more tests.
         */
        FIXME("type %u\n", type);
        *p = 0;
    }

    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_parentNode(IHTMLDOMNode *iface, IHTMLDOMNode **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLDOMNode *node;
    nsIDOMNode *nsnode;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    nsres = nsIDOMNode_GetParentNode(This->nsnode, &nsnode);
    if(NS_FAILED(nsres)) {
        ERR("GetParentNode failed: %08lx\n", nsres);
        return E_FAIL;
    }

    if(!nsnode) {
        *p = NULL;
        return S_OK;
    }

    hres = get_node(nsnode, TRUE, &node);
    nsIDOMNode_Release(nsnode);
    if(FAILED(hres))
        return hres;

    *p = &node->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_hasChildNodes(IHTMLDOMNode *iface, VARIANT_BOOL *fChildren)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    cpp_bool has_child = FALSE;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, fChildren);

    nsres = nsIDOMNode_HasChildNodes(This->nsnode, &has_child);
    if(NS_FAILED(nsres))
        ERR("HasChildNodes failed: %08lx\n", nsres);

    *fChildren = variant_bool(has_child);
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_childNodes(IHTMLDOMNode *iface, IDispatch **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    nsIDOMNodeList *nslist;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    hres = map_nsresult(nsIDOMNode_GetChildNodes(This->nsnode, &nslist));
    if(FAILED(hres)) {
        ERR("GetChildNodes failed: %08lx\n", hres);
        return hres;
    }

    hres = create_child_collection(nslist, This->doc, (IHTMLDOMChildrenCollection**)p);
    nsIDOMNodeList_Release(nslist);
    return hres;
}

static HRESULT WINAPI HTMLDOMNode_get_attributes(IHTMLDOMNode *iface, IDispatch **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLAttributeCollection *col;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    if(This->vtbl->get_attr_col) {
        hres = This->vtbl->get_attr_col(This, &col);
        if(FAILED(hres))
            return hres;

        *p = (IDispatch*)&col->IHTMLAttributeCollection_iface;
        return S_OK;
    }

    *p = NULL;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_insertBefore(IHTMLDOMNode *iface, IHTMLDOMNode *newChild,
                                               VARIANT refChild, IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLDOMNode *new_child, *node_obj, *ref_node = NULL;
    nsIDOMNode *nsnode;
    nsresult nsres;
    HRESULT hres = S_OK;

    TRACE("(%p)->(%p %s %p)\n", This, newChild, debugstr_variant(&refChild), node);

    new_child = get_node_obj(newChild);
    if(!new_child) {
        ERR("invalid newChild\n");
        return E_INVALIDARG;
    }

    switch(V_VT(&refChild)) {
    case VT_NULL:
        break;
    case VT_DISPATCH: {
        IHTMLDOMNode *ref_iface;

        if(!V_DISPATCH(&refChild))
            break;

        hres = IDispatch_QueryInterface(V_DISPATCH(&refChild), &IID_IHTMLDOMNode, (void**)&ref_iface);
        if(FAILED(hres))
            break;

        ref_node = get_node_obj(ref_iface);
        IHTMLDOMNode_Release(ref_iface);
        if(!ref_node) {
            ERR("unvalid node\n");
            hres = E_FAIL;
            break;
        }
        break;
    }
    default:
        FIXME("unimplemented refChild %s\n", debugstr_variant(&refChild));
        hres = E_NOTIMPL;
    }

    if(SUCCEEDED(hres)) {
        nsres = nsIDOMNode_InsertBefore(This->nsnode, new_child->nsnode, ref_node ? ref_node->nsnode : NULL, &nsnode);
        if(NS_FAILED(nsres)) {
            ERR("InsertBefore failed: %08lx\n", nsres);
            hres = E_FAIL;
        }
    }
    node_release(new_child);
    if(ref_node)
        node_release(ref_node);
    if(FAILED(hres))
        return hres;

    hres = get_node(nsnode, TRUE, &node_obj);
    nsIDOMNode_Release(nsnode);
    if(FAILED(hres))
        return hres;

    *node = &node_obj->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_removeChild(IHTMLDOMNode *iface, IHTMLDOMNode *oldChild,
                                              IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLDOMNode *node_obj;
    nsIDOMNode *nsnode;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%p %p)\n", This, oldChild, node);

    node_obj = get_node_obj(oldChild);
    if(!node_obj)
        return E_FAIL;

    nsres = nsIDOMNode_RemoveChild(This->nsnode, node_obj->nsnode, &nsnode);
    node_release(node_obj);
    if(NS_FAILED(nsres)) {
        ERR("RemoveChild failed: %08lx\n", nsres);
        return E_FAIL;
    }

    hres = get_node(nsnode, TRUE, &node_obj);
    nsIDOMNode_Release(nsnode);
    if(FAILED(hres))
        return hres;

    /* FIXME: Make sure that node != newChild */
    *node = &node_obj->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_replaceChild(IHTMLDOMNode *iface, IHTMLDOMNode *newChild,
                                               IHTMLDOMNode *oldChild, IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLDOMNode *node_new, *node_old, *ret_node;
    nsIDOMNode *nsnode;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%p %p %p)\n", This, newChild, oldChild, node);

    node_new = get_node_obj(newChild);
    if(!node_new)
        return E_FAIL;

    node_old = get_node_obj(oldChild);
    if(!node_old) {
        node_release(node_new);
        return E_FAIL;
    }

    nsres = nsIDOMNode_ReplaceChild(This->nsnode, node_new->nsnode, node_old->nsnode, &nsnode);
    node_release(node_new);
    node_release(node_old);
    if(NS_FAILED(nsres))
        return E_FAIL;

    hres = get_node(nsnode, TRUE, &ret_node);
    nsIDOMNode_Release(nsnode);
    if(FAILED(hres))
        return hres;

    *node = &ret_node->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_cloneNode(IHTMLDOMNode *iface, VARIANT_BOOL fDeep,
                                            IHTMLDOMNode **clonedNode)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLDOMNode *new_node;
    nsIDOMNode *nsnode;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%x %p)\n", This, fDeep, clonedNode);

    nsres = nsIDOMNode_CloneNode(This->nsnode, fDeep != VARIANT_FALSE, 1, &nsnode);
    if(NS_FAILED(nsres) || !nsnode) {
        ERR("CloneNode failed: %08lx\n", nsres);
        return E_FAIL;
    }

    hres = This->vtbl->clone(This, nsnode, &new_node);
    nsIDOMNode_Release(nsnode);
    if(FAILED(hres))
        return hres;

    *clonedNode = &new_node->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_removeNode(IHTMLDOMNode *iface, VARIANT_BOOL fDeep,
                                             IHTMLDOMNode **removed)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    FIXME("(%p)->(%x %p)\n", This, fDeep, removed);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode_swapNode(IHTMLDOMNode *iface, IHTMLDOMNode *otherNode,
                                           IHTMLDOMNode **swappedNode)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    FIXME("(%p)->(%p %p)\n", This, otherNode, swappedNode);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode_replaceNode(IHTMLDOMNode *iface, IHTMLDOMNode *replacement,
                                              IHTMLDOMNode **replaced)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    FIXME("(%p)->(%p %p)\n", This, replacement, replaced);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode_appendChild(IHTMLDOMNode *iface, IHTMLDOMNode *newChild,
                                              IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    HTMLDOMNode *node_obj;
    nsIDOMNode *nsnode;
    nsresult nsres;
    HRESULT hres;

    TRACE("(%p)->(%p %p)\n", This, newChild, node);

    node_obj = get_node_obj(newChild);
    if(!node_obj)
        return E_FAIL;

    nsres = nsIDOMNode_AppendChild(This->nsnode, node_obj->nsnode, &nsnode);
    node_release(node_obj);
    if(NS_FAILED(nsres)) {
        ERR("AppendChild failed: %08lx\n", nsres);
        return E_FAIL;
    }

    hres = get_node(nsnode, TRUE, &node_obj);
    nsIDOMNode_Release(nsnode);
    if(FAILED(hres))
        return hres;

    /* FIXME: Make sure that node != newChild */
    *node = &node_obj->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_nodeName(IHTMLDOMNode *iface, BSTR *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    nsAString name;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    nsAString_Init(&name, NULL);
    nsres = nsIDOMNode_GetNodeName(This->nsnode, &name);
    return return_nsstr(nsres, &name, p);
}

static HRESULT WINAPI HTMLDOMNode_put_nodeValue(IHTMLDOMNode *iface, VARIANT v)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    switch(V_VT(&v)) {
    case VT_BSTR: {
        nsAString val_str;

        nsAString_InitDepend(&val_str, V_BSTR(&v));
        nsIDOMNode_SetNodeValue(This->nsnode, &val_str);
        nsAString_Finish(&val_str);

        return S_OK;
    }

    default:
        FIXME("unsupported value %s\n", debugstr_variant(&v));
    }

    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode_get_nodeValue(IHTMLDOMNode *iface, VARIANT *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    const PRUnichar *val;
    nsAString val_str;

    TRACE("(%p)->(%p)\n", This, p);

    nsAString_Init(&val_str, NULL);
    nsIDOMNode_GetNodeValue(This->nsnode, &val_str);
    nsAString_GetData(&val_str, &val);

    if(*val) {
        V_VT(p) = VT_BSTR;
        V_BSTR(p) = SysAllocString(val);
    }else {
        V_VT(p) = VT_NULL;
    }

    nsAString_Finish(&val_str);

    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_firstChild(IHTMLDOMNode *iface, IHTMLDOMNode **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    nsIDOMNode *nschild = NULL;
    HTMLDOMNode *node;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    nsIDOMNode_GetFirstChild(This->nsnode, &nschild);
    if(!nschild) {
        *p = NULL;
        return S_OK;
    }

    hres = get_node(nschild, TRUE, &node);
    nsIDOMNode_Release(nschild);
    if(FAILED(hres))
        return hres;

    *p = &node->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_lastChild(IHTMLDOMNode *iface, IHTMLDOMNode **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    nsIDOMNode *nschild = NULL;
    HTMLDOMNode *node;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    nsIDOMNode_GetLastChild(This->nsnode, &nschild);
    if(!nschild) {
        *p = NULL;
        return S_OK;
    }

    hres = get_node(nschild, TRUE, &node);
    nsIDOMNode_Release(nschild);
    if(FAILED(hres))
        return hres;

    *p = &node->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_previousSibling(IHTMLDOMNode *iface, IHTMLDOMNode **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    nsIDOMNode *nschild = NULL;
    HTMLDOMNode *node;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    nsIDOMNode_GetPreviousSibling(This->nsnode, &nschild);
    if(!nschild) {
        *p = NULL;
        return S_OK;
    }

    hres = get_node(nschild, TRUE, &node);
    nsIDOMNode_Release(nschild);
    if(FAILED(hres))
        return hres;

    *p = &node->IHTMLDOMNode_iface;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode_get_nextSibling(IHTMLDOMNode *iface, IHTMLDOMNode **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(iface);
    nsIDOMNode *nssibling = NULL;
    HTMLDOMNode *node;
    HRESULT hres;

    TRACE("(%p)->(%p)\n", This, p);

    nsIDOMNode_GetNextSibling(This->nsnode, &nssibling);
    if(!nssibling) {
        *p = NULL;
        return S_OK;
    }

    hres = get_node(nssibling, TRUE, &node);
    nsIDOMNode_Release(nssibling);
    if(FAILED(hres))
        return hres;

    *p = &node->IHTMLDOMNode_iface;
    return S_OK;
}

static const IHTMLDOMNodeVtbl HTMLDOMNodeVtbl = {
    HTMLDOMNode_QueryInterface,
    HTMLDOMNode_AddRef,
    HTMLDOMNode_Release,
    HTMLDOMNode_GetTypeInfoCount,
    HTMLDOMNode_GetTypeInfo,
    HTMLDOMNode_GetIDsOfNames,
    HTMLDOMNode_Invoke,
    HTMLDOMNode_get_nodeType,
    HTMLDOMNode_get_parentNode,
    HTMLDOMNode_hasChildNodes,
    HTMLDOMNode_get_childNodes,
    HTMLDOMNode_get_attributes,
    HTMLDOMNode_insertBefore,
    HTMLDOMNode_removeChild,
    HTMLDOMNode_replaceChild,
    HTMLDOMNode_cloneNode,
    HTMLDOMNode_removeNode,
    HTMLDOMNode_swapNode,
    HTMLDOMNode_replaceNode,
    HTMLDOMNode_appendChild,
    HTMLDOMNode_get_nodeName,
    HTMLDOMNode_put_nodeValue,
    HTMLDOMNode_get_nodeValue,
    HTMLDOMNode_get_firstChild,
    HTMLDOMNode_get_lastChild,
    HTMLDOMNode_get_previousSibling,
    HTMLDOMNode_get_nextSibling
};

HTMLDOMNode *unsafe_impl_from_IHTMLDOMNode(IHTMLDOMNode *iface)
{
    return iface->lpVtbl == &HTMLDOMNodeVtbl ? impl_from_IHTMLDOMNode(iface) : NULL;
}

static HTMLDOMNode *get_node_obj(IHTMLDOMNode *iface)
{
    HTMLDOMNode *ret;

    if(iface->lpVtbl != &HTMLDOMNodeVtbl)
        return NULL;

    ret = impl_from_IHTMLDOMNode(iface);
    node_addref(ret);
    return ret;
}

static inline HTMLDOMNode *impl_from_IHTMLDOMNode2(IHTMLDOMNode2 *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMNode, IHTMLDOMNode2_iface);
}

static HRESULT WINAPI HTMLDOMNode2_QueryInterface(IHTMLDOMNode2 *iface,
        REFIID riid, void **ppv)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);

    return IHTMLDOMNode_QueryInterface(&This->IHTMLDOMNode_iface, riid, ppv);
}

static ULONG WINAPI HTMLDOMNode2_AddRef(IHTMLDOMNode2 *iface)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);

    return IHTMLDOMNode_AddRef(&This->IHTMLDOMNode_iface);
}

static ULONG WINAPI HTMLDOMNode2_Release(IHTMLDOMNode2 *iface)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);

    return IHTMLDOMNode_Release(&This->IHTMLDOMNode_iface);
}

static HRESULT WINAPI HTMLDOMNode2_GetTypeInfoCount(IHTMLDOMNode2 *iface, UINT *pctinfo)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLDOMNode2_GetTypeInfo(IHTMLDOMNode2 *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);
    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLDOMNode2_GetIDsOfNames(IHTMLDOMNode2 *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);
    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLDOMNode2_Invoke(IHTMLDOMNode2 *iface, DISPID dispIdMember,
        REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);
    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLDOMNode2_get_ownerDocument(IHTMLDOMNode2 *iface, IDispatch **p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode2(iface);

    TRACE("(%p)->(%p)\n", This, p);

    /* FIXME: Better check for document node */
    if(This == &This->doc->node) {
        *p = NULL;
    }else {
        *p = (IDispatch*)&This->doc->basedoc.IHTMLDocument2_iface;
        IDispatch_AddRef(*p);
    }
    return S_OK;
}

static const IHTMLDOMNode2Vtbl HTMLDOMNode2Vtbl = {
    HTMLDOMNode2_QueryInterface,
    HTMLDOMNode2_AddRef,
    HTMLDOMNode2_Release,
    HTMLDOMNode2_GetTypeInfoCount,
    HTMLDOMNode2_GetTypeInfo,
    HTMLDOMNode2_GetIDsOfNames,
    HTMLDOMNode2_Invoke,
    HTMLDOMNode2_get_ownerDocument
};

static inline HTMLDOMNode *impl_from_IHTMLDOMNode3(IHTMLDOMNode3 *iface)
{
    return CONTAINING_RECORD(iface, HTMLDOMNode, IHTMLDOMNode3_iface);
}

static HRESULT WINAPI HTMLDOMNode3_QueryInterface(IHTMLDOMNode3 *iface, REFIID riid, void **ppv)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    return IHTMLDOMNode_QueryInterface(&This->IHTMLDOMNode_iface, riid, ppv);
}

static ULONG WINAPI HTMLDOMNode3_AddRef(IHTMLDOMNode3 *iface)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);

    return IHTMLDOMNode_AddRef(&This->IHTMLDOMNode_iface);
}

static ULONG WINAPI HTMLDOMNode3_Release(IHTMLDOMNode3 *iface)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);

    return IHTMLDOMNode_Release(&This->IHTMLDOMNode_iface);
}

static HRESULT WINAPI HTMLDOMNode3_GetTypeInfoCount(IHTMLDOMNode3 *iface, UINT *pctinfo)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    return IDispatchEx_GetTypeInfoCount(&This->event_target.dispex.IDispatchEx_iface, pctinfo);
}

static HRESULT WINAPI HTMLDOMNode3_GetTypeInfo(IHTMLDOMNode3 *iface, UINT iTInfo,
        LCID lcid, ITypeInfo **ppTInfo)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    return IDispatchEx_GetTypeInfo(&This->event_target.dispex.IDispatchEx_iface, iTInfo, lcid, ppTInfo);
}

static HRESULT WINAPI HTMLDOMNode3_GetIDsOfNames(IHTMLDOMNode3 *iface, REFIID riid,
                                                LPOLESTR *rgszNames, UINT cNames,
                                                LCID lcid, DISPID *rgDispId)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    return IDispatchEx_GetIDsOfNames(&This->event_target.dispex.IDispatchEx_iface, riid, rgszNames, cNames,
            lcid, rgDispId);
}

static HRESULT WINAPI HTMLDOMNode3_Invoke(IHTMLDOMNode3 *iface, DISPID dispIdMember,
        REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
        VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    return IDispatchEx_Invoke(&This->event_target.dispex.IDispatchEx_iface, dispIdMember, riid, lcid,
            wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);
}

static HRESULT WINAPI HTMLDOMNode3_put_prefix(IHTMLDOMNode3 *iface, VARIANT v)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->(%s)\n", This, debugstr_variant(&v));
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_get_prefix(IHTMLDOMNode3 *iface, VARIANT *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_get_localName(IHTMLDOMNode3 *iface, VARIANT *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->(%p)\n", This, p);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_get_namespaceURI(IHTMLDOMNode3 *iface, VARIANT *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    nsAString_InitDepend(&nsstr, NULL);
    nsres = nsIDOMNode_GetNamespaceURI(This->nsnode, &nsstr);
    return return_nsstr_variant(nsres, &nsstr, 0, p);
}

static HRESULT WINAPI HTMLDOMNode3_put_textContent(IHTMLDOMNode3 *iface, VARIANT v)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%s)\n", This, debugstr_variant(&v));

    variant_to_nsstr(&v, FALSE, &nsstr);
    nsres = nsIDOMNode_SetTextContent(This->nsnode, &nsstr);
    nsAString_Finish(&nsstr);
    if(NS_FAILED(nsres)) {
        ERR("SetTextContent failed: %08lx\n", nsres);
        return E_FAIL;
    }

    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode3_get_textContent(IHTMLDOMNode3 *iface, VARIANT *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    nsAString nsstr;
    nsresult nsres;

    TRACE("(%p)->(%p)\n", This, p);

    nsAString_Init(&nsstr, NULL);
    nsres = nsIDOMNode_GetTextContent(This->nsnode, &nsstr);
    return return_nsstr_variant(nsres, &nsstr, 0, p);
}

static HRESULT WINAPI HTMLDOMNode3_isEqualNode(IHTMLDOMNode3 *iface, IHTMLDOMNode3 *otherNode, VARIANT_BOOL *isEqual)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_lookupNamespaceURI(IHTMLDOMNode3 *iface, VARIANT *prefix, VARIANT *namespaceURI)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->(%s %p)\n", This, debugstr_variant(prefix), namespaceURI);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_lookupPrefix(IHTMLDOMNode3 *iface, VARIANT *namespaceURI, VARIANT *prefix)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->(%s %p)\n", This, debugstr_variant(namespaceURI), prefix);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_isDefaultNamespace(IHTMLDOMNode3 *iface, VARIANT *namespace, VARIANT_BOOL *pfDefaultNamespace)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_appendChild(IHTMLDOMNode3 *iface, IHTMLDOMNode *newChild, IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    TRACE("(%p)->()\n", This);
    return IHTMLDOMNode_appendChild(&This->IHTMLDOMNode_iface, newChild, node);
}

static HRESULT WINAPI HTMLDOMNode3_insertBefore(IHTMLDOMNode3 *iface, IHTMLDOMNode *newChild, VARIANT refChild, IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    TRACE("(%p)->(%p %s %p)\n", This, newChild, debugstr_variant(&refChild), node);
    return IHTMLDOMNode_insertBefore(&This->IHTMLDOMNode_iface, newChild, refChild, node);
}

static HRESULT WINAPI HTMLDOMNode3_removeChild(IHTMLDOMNode3 *iface, IHTMLDOMNode *oldChild, IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    TRACE("(%p)->(%p %p)\n", This, oldChild, node);
    return IHTMLDOMNode_removeChild(&This->IHTMLDOMNode_iface, oldChild, node);
}

static HRESULT WINAPI HTMLDOMNode3_replaceChild(IHTMLDOMNode3 *iface, IHTMLDOMNode *newChild, IHTMLDOMNode *oldChild, IHTMLDOMNode **node)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    TRACE("(%p)->(%p %p %p)\n", This, newChild, oldChild, node);
    return IHTMLDOMNode_replaceChild(&This->IHTMLDOMNode_iface, newChild, oldChild, node);
}

static HRESULT WINAPI HTMLDOMNode3_isSameNode(IHTMLDOMNode3 *iface, IHTMLDOMNode3 *otherNode, VARIANT_BOOL *isSame)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->()\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI HTMLDOMNode3_compareDocumentPosition(IHTMLDOMNode3 *iface, IHTMLDOMNode *otherNode, USHORT *flags)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    HTMLDOMNode *other;
    UINT16 position;
    nsresult nsres;

    TRACE("(%p)->()\n", This);

    other = get_node_obj(otherNode);
    if(!other)
        return E_INVALIDARG;

    nsres = nsIDOMNode_CompareDocumentPosition(This->nsnode, other->nsnode, &position);
    IHTMLDOMNode_Release(&other->IHTMLDOMNode_iface);
    if(NS_FAILED(nsres)) {
        ERR("failed: %08lx\n", nsres);
        return E_FAIL;
    }

    *flags = position;
    return S_OK;
}

static HRESULT WINAPI HTMLDOMNode3_isSupported(IHTMLDOMNode3 *iface, BSTR feature, VARIANT version, VARIANT_BOOL *pfisSupported)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode3(iface);
    FIXME("(%p)->(%s %s %p)\n", This, debugstr_w(feature), debugstr_variant(&version), pfisSupported);
    return E_NOTIMPL;
}

static const IHTMLDOMNode3Vtbl HTMLDOMNode3Vtbl = {
    HTMLDOMNode3_QueryInterface,
    HTMLDOMNode3_AddRef,
    HTMLDOMNode3_Release,
    HTMLDOMNode3_GetTypeInfoCount,
    HTMLDOMNode3_GetTypeInfo,
    HTMLDOMNode3_GetIDsOfNames,
    HTMLDOMNode3_Invoke,
    HTMLDOMNode3_put_prefix,
    HTMLDOMNode3_get_prefix,
    HTMLDOMNode3_get_localName,
    HTMLDOMNode3_get_namespaceURI,
    HTMLDOMNode3_put_textContent,
    HTMLDOMNode3_get_textContent,
    HTMLDOMNode3_isEqualNode,
    HTMLDOMNode3_lookupNamespaceURI,
    HTMLDOMNode3_lookupPrefix,
    HTMLDOMNode3_isDefaultNamespace,
    HTMLDOMNode3_appendChild,
    HTMLDOMNode3_insertBefore,
    HTMLDOMNode3_removeChild,
    HTMLDOMNode3_replaceChild,
    HTMLDOMNode3_isSameNode,
    HTMLDOMNode3_compareDocumentPosition,
    HTMLDOMNode3_isSupported
};

HRESULT HTMLDOMNode_QI(HTMLDOMNode *This, REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s %p)\n", This, debugstr_mshtml_guid(riid), ppv);

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        *ppv = &This->IHTMLDOMNode_iface;
    }else if(IsEqualGUID(&IID_IDispatch, riid)) {
        *ppv = &This->IHTMLDOMNode_iface;
    }else if(IsEqualGUID(&IID_IHTMLDOMNode, riid)) {
        *ppv = &This->IHTMLDOMNode_iface;
    }else if(IsEqualGUID(&IID_IHTMLDOMNode2, riid)) {
        *ppv = &This->IHTMLDOMNode2_iface;
    }else if(IsEqualGUID(&IID_IHTMLDOMNode3, riid)) {
        *ppv = &This->IHTMLDOMNode3_iface;
    }else if(IsEqualGUID(&IID_nsXPCOMCycleCollectionParticipant, riid)) {
        *ppv = &node_ccp;
        return S_OK;
    }else if(IsEqualGUID(&IID_nsCycleCollectionISupports, riid)) {
        *ppv = &This->IHTMLDOMNode_iface;
        return S_OK;
    }else {
        return EventTarget_QI(&This->event_target, riid, ppv);
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

void HTMLDOMNode_destructor(HTMLDOMNode *This)
{
    release_event_target(&This->event_target);
    if(This->nsnode)
        nsIDOMNode_Release(This->nsnode);
    if(This->doc && &This->doc->node != This)
        htmldoc_release(&This->doc->basedoc);
}

static HRESULT HTMLDOMNode_clone(HTMLDOMNode *This, nsIDOMNode *nsnode, HTMLDOMNode **ret)
{
    return create_node(This->doc, nsnode, ret);
}

void HTMLDOMNode_init_dispex_info(dispex_data_t *info, compat_mode_t mode)
{
    if(mode >= COMPAT_MODE_IE9) {
        dispex_info_add_interface(info, IHTMLDOMNode2_tid, NULL);
        dispex_info_add_interface(info, IHTMLDOMNode3_tid, NULL);
    }

    EventTarget_init_dispex_info(info, mode);
}

static const cpc_entry_t HTMLDOMNode_cpc[] = {{NULL}};

static const NodeImplVtbl HTMLDOMNodeImplVtbl = {
    NULL,
    HTMLDOMNode_QI,
    HTMLDOMNode_destructor,
    HTMLDOMNode_cpc,
    HTMLDOMNode_clone
};

void HTMLDOMNode_Init(HTMLDocumentNode *doc, HTMLDOMNode *node, nsIDOMNode *nsnode, dispex_static_data_t *dispex_data)
{
    nsresult nsres;

    node->IHTMLDOMNode_iface.lpVtbl = &HTMLDOMNodeVtbl;
    node->IHTMLDOMNode2_iface.lpVtbl = &HTMLDOMNode2Vtbl;
    node->IHTMLDOMNode3_iface.lpVtbl = &HTMLDOMNode3Vtbl;

    ccref_init(&node->ccref, 1);
    EventTarget_Init(&node->event_target, (IUnknown*)&node->IHTMLDOMNode_iface, dispex_data, doc);

    if(&doc->node != node)
        htmldoc_addref(&doc->basedoc);
    node->doc = doc;

    nsIDOMNode_AddRef(nsnode);
    node->nsnode = nsnode;

    nsres = nsIDOMNode_SetMshtmlNode(nsnode, (nsISupports*)&node->IHTMLDOMNode_iface);
    assert(nsres == NS_OK);
}

static const tid_t HTMLDOMNode_iface_tids[] = {
    IHTMLDOMNode_tid,
    0
};
dispex_static_data_t HTMLDOMNode_dispex = {
    L"Node",
    NULL,
    PROTO_ID_HTMLDOMNode,
    IHTMLDOMNode_tid,
    HTMLDOMNode_iface_tids,
    HTMLDOMNode_init_dispex_info
};

static HRESULT create_node(HTMLDocumentNode *doc, nsIDOMNode *nsnode, HTMLDOMNode **ret)
{
    UINT16 node_type;
    HRESULT hres;

    nsIDOMNode_GetNodeType(nsnode, &node_type);

    switch(node_type) {
    case ELEMENT_NODE: {
        HTMLElement *elem;
        hres = HTMLElement_Create(doc, nsnode, FALSE, &elem);
        if(FAILED(hres))
            return hres;
        *ret = &elem->node;
        break;
    }
    case TEXT_NODE:
        hres = HTMLDOMTextNode_Create(doc, nsnode, ret);
        if(FAILED(hres))
            return hres;
        break;
    /* doctype nodes are represented as comment nodes (at least in quirks mode) */
    case DOCUMENT_TYPE_NODE:
    case COMMENT_NODE: {
        HTMLElement *comment;
        hres = HTMLCommentElement_Create(doc, nsnode, &comment);
        if(FAILED(hres))
            return hres;
        *ret = &comment->node;
        break;
    }
    case ATTRIBUTE_NODE:
        ERR("Called on attribute node\n");
        return E_UNEXPECTED;
    default: {
        HTMLDOMNode *node;

        FIXME("unimplemented node type %u\n", node_type);

        node = heap_alloc_zero(sizeof(HTMLDOMNode));
        if(!node)
            return E_OUTOFMEMORY;

        node->vtbl = &HTMLDOMNodeImplVtbl;
        HTMLDOMNode_Init(doc, node, nsnode, &HTMLDOMNode_dispex);
        *ret = node;
    }
    }

    TRACE("type %d ret %p\n", node_type, *ret);
    return S_OK;
}

static nsresult NSAPI HTMLDOMNode_traverse(void *ccp, void *p, nsCycleCollectionTraversalCallback *cb)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(p);

    TRACE("%p\n", This);

    describe_cc_node(&This->ccref, "HTMLDOMNode", cb);

    if(This->nsnode)
        note_cc_edge((nsISupports*)This->nsnode, "This->nsnode", cb);
    if(This->doc && &This->doc->node != This)
        note_cc_edge((nsISupports*)&This->doc->node.IHTMLDOMNode_iface, "This->doc", cb);
    dispex_traverse(&This->event_target.dispex, cb);

    if(This->vtbl->traverse)
        This->vtbl->traverse(This, cb);

    return NS_OK;
}

static nsresult NSAPI HTMLDOMNode_unlink(void *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(p);

    TRACE("%p\n", This);

    if(This->vtbl->unlink)
        This->vtbl->unlink(This);

    dispex_unlink(&This->event_target.dispex);

    if(This->nsnode) {
        nsIDOMNode *nsnode = This->nsnode;
        This->nsnode = NULL;
        nsIDOMNode_Release(nsnode);
    }

    if(This->doc && &This->doc->node != This) {
        HTMLDocumentNode *doc = This->doc;
        This->doc = NULL;
        htmldoc_release(&doc->basedoc);
    }else {
        This->doc = NULL;
    }

    return NS_OK;
}

static void NSAPI HTMLDOMNode_delete_cycle_collectable(void *p)
{
    HTMLDOMNode *This = impl_from_IHTMLDOMNode(p);

    TRACE("(%p)\n", This);

    if(This->vtbl->unlink)
        This->vtbl->unlink(This);
    This->vtbl->destructor(This);
    release_dispex(&This->event_target.dispex);
    heap_free(This);
}

void init_node_cc(void)
{
    static const CCObjCallback node_ccp_callback = {
        HTMLDOMNode_traverse,
        HTMLDOMNode_unlink,
        HTMLDOMNode_delete_cycle_collectable
    };

    ccp_init(&node_ccp, &node_ccp_callback);
}

HRESULT get_node(nsIDOMNode *nsnode, BOOL create, HTMLDOMNode **ret)
{
    nsIDOMDocument *dom_document;
    HTMLDocumentNode *document;
    nsISupports *unk = NULL;
    nsresult nsres;
    HRESULT hres;

    nsres = nsIDOMNode_GetMshtmlNode(nsnode, &unk);
    assert(nsres == NS_OK);

    if(unk) {
        *ret = get_node_obj((IHTMLDOMNode*)unk);
        nsISupports_Release(unk);
        return NS_OK;
    }

    if(!create) {
        *ret = NULL;
        return S_OK;
    }

    nsres = nsIDOMNode_GetOwnerDocument(nsnode, &dom_document);
    if(NS_FAILED(nsres) || !dom_document) {
        ERR("GetOwnerDocument failed: %08lx\n", nsres);
        return E_FAIL;
    }

    hres = get_document_node(dom_document, &document);
    nsIDOMDocument_Release(dom_document);
    if(!document)
        return E_FAIL;

    hres = create_node(document, nsnode, ret);
    htmldoc_release(&document->basedoc);
    return hres;
}
