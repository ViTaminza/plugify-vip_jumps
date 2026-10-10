#pragma once
// The generated vip bindings (the consumer API of plugins/vip, produced by plugify-gen from
// vip.pplugin) declare the manifest's prototypes as plain function-pointer aliases inside
// namespace `vip`; the hand-written consumer wrapper (plugins/vip/include/vip_api.hpp) declares
// the *same* names there as std::function wrappers, so including <vip_api.hpp> alone does not
// compile ("conflicting declaration"). The colliding aliases are renamed on the way in here:
// the wrapper's trampolines are plain function pointers and convert implicitly to the generated
// pointer types, so nothing in this plugin ever needs the renamed names.
//
// If the core renames its prototypes (or the wrapper its aliases), these renames simply stop
// matching anything and the header keeps working. Reported upstream; do not edit the vip core
// from here.
#define ItemSelectableCallback vip_boundary_ItemSelectableCallback
#define ItemTogglableCallback vip_boundary_ItemTogglableCallback
#define ItemDisplayCallback vip_boundary_ItemDisplayCallback
#define ReadyCallbackFunc vip_boundary_ReadyCallbackFunc
#define SpawnCallbackFunc vip_boundary_SpawnCallbackFunc
#define ClientLoadedOrDisconnectCallbackFunc vip_boundary_ClientLoadedOrDisconnectCallbackFunc
#define VIPRemoveCallbackFunc vip_boundary_VIPRemoveCallbackFunc
#define VIPAddCallbackFunc vip_boundary_VIPAddCallbackFunc

#include <vip/vip.hpp>

#undef ItemSelectableCallback
#undef ItemTogglableCallback
#undef ItemDisplayCallback
#undef ReadyCallbackFunc
#undef SpawnCallbackFunc
#undef ClientLoadedOrDisconnectCallbackFunc
#undef VIPRemoveCallbackFunc
#undef VIPAddCallbackFunc

#include <vip_api.hpp>
