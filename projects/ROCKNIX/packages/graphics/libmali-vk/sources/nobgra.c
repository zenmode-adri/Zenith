/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Zenith project
 *
 * VK_LAYER_R36_nobgra
 *  1. Oculta B8G8R8A8 (44/50) de vkGetPhysicalDeviceSurfaceFormatsKHR para que PPSSPP elija R8G8B8A8.
 *     Motivo: la capa WSI de Arm no puede crear swapchain BGRA en sway (ARGB8888 solo con modificador implicito).
 *  2. Espera a la GPU (vkQueueWaitIdle) antes de cada vkQueuePresentKHR. Motivo: el Vulkan de Arm entrega la imagen
 *     a sway sin esperar a que la GPU termine de dibujarla (sin sincronizacion explicita): salian fotogramas a medio
 *     dibujar («cubos»/tearing). Activo de fabrica; apagar con waitidle=0 en /storage/.config/r36vk.conf.
 *
 * Opciones de depuracion (clave=valor en /storage/.config/r36vk.conf, o la ruta de R36VK_CONF), apagadas por defecto:
 *   waitidle=0|1 (1 por defecto), minimages=N, present=fifo|mailbox|immediate, opaque=1.
 *
 * Encadenado: patron estandar del cargador (VK_LAYER_LINK_INFO), con una tabla por instancia y otra por dispositivo
 * (clave = puntero de despacho del objeto), para convivir con otras capas (framelog, MangoHud...).
 * Solo se devuelven nuestras funciones para lo que se intercepta; lo demas lo resuelve la siguiente capa. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#ifdef TRACE
#define TR(...) do { fprintf(stderr, "[r36vk-T] " __VA_ARGS__); fputc(10, stderr); } while (0)
#else
#define TR(...) do {} while (0)
#endif
#define EXPORT __attribute__((visibility("default")))

typedef struct inst_s {
  void *key;
  VkInstance h;
  struct inst_s *next;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes;
} inst_t;

typedef struct dev_s {
  void *key;
  struct dev_s *next;
  VkPhysicalDevice pd;
  inst_t *inst;
  PFN_vkGetDeviceProcAddr gdpa;
  PFN_vkCreateSwapchainKHR create_sc;
  PFN_vkQueuePresentKHR present;
  PFN_vkQueueWaitIdle waitidle;
} vdev_t;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static inst_t *insts;
static vdev_t *devs;

static void *dkey(const void *h) { return h ? *(void *const *)h : NULL; }

static inst_t *inst_find(const void *h) {
  void *k = dkey(h); inst_t *r = NULL;
  pthread_mutex_lock(&lk);
  for (inst_t *i = insts; i; i = i->next) if (i->key == k) { r = i; break; }
  if (!r) r = insts;
  pthread_mutex_unlock(&lk);
  return r;
}
static vdev_t *dev_find(const void *h) {
  void *k = dkey(h); vdev_t *r = NULL;
  pthread_mutex_lock(&lk);
  for (vdev_t *d = devs; d; d = d->next) if (d->key == k) { r = d; break; }
  if (!r) r = devs;  /* una sola instancia/dispositivo en la practica: respaldo si la clave no coincide */
  pthread_mutex_unlock(&lk);
  return r;
}

/* ---- opciones ---- */
static int c_waitidle = 1, c_minimages, c_opaque, c_present = -1, c_loaded, c_file;
static const char *c_path;
static void say(const char *fmt, ...) {
  char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  fprintf(stderr, "[r36vk] %s\n", b); syslog(LOG_INFO, "[r36vk] %s", b);
}
static void load_conf(void) {
  pthread_mutex_lock(&lk);
  if (c_loaded) { pthread_mutex_unlock(&lk); return; }
  c_loaded = 1;
  pthread_mutex_unlock(&lk);
  c_path = getenv("R36VK_CONF") ? getenv("R36VK_CONF") : "/storage/.config/r36vk.conf";
  FILE *f = fopen(c_path, "r");
  if (!f) return;
  c_file = 1;
  char l[128];
  while (fgets(l, sizeof l, f)) {
    char *e = strchr(l, '='); if (!e || l[0] == '#') continue;
    *e++ = 0; e[strcspn(e, "\r\n ")] = 0;
    if (!strcmp(l, "waitidle")) c_waitidle = atoi(e);
    else if (!strcmp(l, "minimages")) c_minimages = atoi(e);
    else if (!strcmp(l, "opaque")) c_opaque = atoi(e);
    else if (!strcmp(l, "present")) c_present = !strcmp(e, "fifo") ? VK_PRESENT_MODE_FIFO_KHR : !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : -1;
  }
  fclose(f);
}

/* ---- formatos ---- */
static int is_bgra(const VkSurfaceFormatKHR *f) {
  return f->format == VK_FORMAT_B8G8R8A8_UNORM || f->format == VK_FORMAT_B8G8R8A8_SRGB;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_formats(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *pCount, VkSurfaceFormatKHR *pF) {
  inst_t *in = inst_find(pd);
  if (!in || !in->formats) return VK_ERROR_INITIALIZATION_FAILED;
  uint32_t n = 0;
  VkResult r = in->formats(pd, s, &n, NULL);
  if (r != VK_SUCCESS) return r;
  VkSurfaceFormatKHR *all = (VkSurfaceFormatKHR *)malloc(sizeof(*all) * (n ? n : 1));
  if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
  r = in->formats(pd, s, &n, all);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) { free(all); return r; }
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; i++)
    if (!is_bgra(&all[i])) m++;
  /* si ocultar BGRA no dejara ningun formato, se devuelve la lista original (mejor BGRA que una swapchain imposible) */
  int filter = m > 0;
  if (!filter) m = n;
  if (!pF) { *pCount = m; free(all); return VK_SUCCESS; }
  uint32_t k = *pCount < m ? *pCount : m;
  uint32_t j = 0;
  for (uint32_t i = 0; i < n && j < k; i++)
    if (!filter || !is_bgra(&all[i])) pF[j++] = all[i];
  *pCount = k;
  free(all);
  return k < m ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- swapchain y present ---- */
static VKAPI_ATTR VkResult VKAPI_CALL my_CreateSwapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
  vdev_t *dv = dev_find(d);
  if (!dv || !dv->create_sc) return VK_ERROR_INITIALIZATION_FAILED;
  TR("createsc d=%p dv=%p", (void *)d, (void *)dv);
  VkSwapchainCreateInfoKHR c = *ci;
  load_conf();
  inst_t *in = dv->inst;
  if (c_minimages > 0 && in && in->caps && dv->pd) {
    VkSurfaceCapabilitiesKHR cap;
    uint32_t n = (uint32_t)c_minimages;
    if (in->caps(dv->pd, ci->surface, &cap) == VK_SUCCESS && cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    if (n > c.minImageCount) c.minImageCount = n;
  }
  if (c_present >= 0 && in && in->modes && dv->pd) {
    uint32_t n = 0; VkPresentModeKHR m[8];
    in->modes(dv->pd, ci->surface, &n, NULL); if (n > 8) n = 8;
    in->modes(dv->pd, ci->surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if ((int)m[i] == c_present) { c.presentMode = (VkPresentModeKHR)c_present; break; }
  }
  if (c_opaque) c.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  say("swapchain: waitidle=%d%s min %u->%u present %d->%d fmt %d %ux%u", c_waitidle, c_file ? " (conf)" : "",
      ci->minImageCount, c.minImageCount, ci->presentMode, c.presentMode, ci->imageFormat, ci->imageExtent.width, ci->imageExtent.height);
  VkResult r = dv->create_sc(d, &c, a, out);
  if (r != VK_SUCCESS && (c_opaque || c_present >= 0 || c_minimages > 0)) {
    say("swapchain con cambios fallo (%d): reintento con los valores originales", r);
    r = dv->create_sc(d, ci, a, out);
  }
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_Present(VkQueue q, const VkPresentInfoKHR *pi) {
  vdev_t *dv = dev_find(q);
  TR("present q=%p dv=%p", (void *)q, (void *)dv);
  if (!dv || !dv->present) return VK_ERROR_INITIALIZATION_FAILED;
  if (c_waitidle && dv->waitidle) dv->waitidle(q);
  return dv->present(q, pi);
}

/* ---- dispositivo ---- */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gdpa(VkDevice d, const char *name) {
  vdev_t *dv = dev_find(d);
  TR("gdpa(%p,%s) dv=%p", (void *)d, name, (void *)dv);
  if (!dv || !dv->gdpa) return NULL;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)my_gdpa;
  if (dv->create_sc && !strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)my_CreateSwapchain;
  if (dv->present && !strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)my_Present;
  return dv->gdpa(d, name);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
  VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
  while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
    chain = (VkLayerDeviceCreateInfo *)chain->pNext;
  inst_t *in = inst_find(pd);
  if (!chain || !in) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr ngipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr ngdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  PFN_vkCreateDevice next_cd = (PFN_vkCreateDevice)ngipa(in->h, "vkCreateDevice");
  if (!next_cd) return VK_ERROR_INITIALIZATION_FAILED;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;  /* avanzar la cadena para la capa siguiente */
  VkResult r = next_cd(pd, ci, a, out);
  if (r != VK_SUCCESS) return r;
  vdev_t *dv = (vdev_t *)calloc(1, sizeof *dv);
  if (!dv) return r;
  dv->key = dkey(*out); dv->pd = pd; dv->inst = in; dv->gdpa = ngdpa;
  dv->create_sc = (PFN_vkCreateSwapchainKHR)ngdpa(*out, "vkCreateSwapchainKHR");
  dv->present = (PFN_vkQueuePresentKHR)ngdpa(*out, "vkQueuePresentKHR");
  dv->waitidle = (PFN_vkQueueWaitIdle)ngdpa(*out, "vkQueueWaitIdle");
  pthread_mutex_lock(&lk);
  for (vdev_t **p = &devs; *p; p = &(*p)->next) if ((*p)->key == dv->key) { vdev_t *old = *p; *p = old->next; free(old); break; }
  dv->next = devs; devs = dv;
  pthread_mutex_unlock(&lk);
  return r;
}

/* ---- instancia ---- */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance i, const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL my_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
  VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
  while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
    chain = (VkLayerInstanceCreateInfo *)chain->pNext;
  if (!chain) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr ngipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkCreateInstance next_ci = (PFN_vkCreateInstance)ngipa(NULL, "vkCreateInstance");
  if (!next_ci) return VK_ERROR_INITIALIZATION_FAILED;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;  /* avanzar la cadena para la capa siguiente */
  VkResult r = next_ci(ci, a, out);
  if (r != VK_SUCCESS) return r;
  inst_t *in = (inst_t *)calloc(1, sizeof *in);
  if (!in) return r;
  in->key = dkey(*out); in->h = *out; in->gipa = ngipa;
  in->formats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)ngipa(*out, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  in->caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)ngipa(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  in->modes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)ngipa(*out, "vkGetPhysicalDeviceSurfacePresentModesKHR");
  pthread_mutex_lock(&lk);
  for (inst_t **p = &insts; *p; p = &(*p)->next) if ((*p)->key == in->key) { inst_t *old = *p; *p = old->next; free(old); break; }
  in->next = insts; insts = in;
  pthread_mutex_unlock(&lk);
  return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_gipa(VkInstance i, const char *name) {
  if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)my_CreateInstance;
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)my_gipa;
  /* capas antiguas (p. ej. la framelog) piden funciones de instancia con VK_NULL_HANDLE (vkCreateDevice): se resuelven por la
   * instancia conocida (una sola en la practica) en vez de devolver NULL, que les haria llamar a un puntero nulo */
  inst_t *in = i ? inst_find(i) : NULL;
  if (!in) { pthread_mutex_lock(&lk); in = insts; pthread_mutex_unlock(&lk); }
  TR("gipa(%p,%s) in=%p", (void *)i, name, (void *)in);
  if (!in || !in->gipa) return NULL;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)my_gdpa;
  if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)my_CreateDevice;
  if (in->formats && !strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR")) return (PFN_vkVoidFunction)my_formats;
  return in->gipa(i, name);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
  if (v->loaderLayerInterfaceVersion >= 2) {
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = my_gipa;
    v->pfnGetDeviceProcAddr = my_gdpa;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
  }
  return VK_SUCCESS;
}
