//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

// to compile this on raspi execute the following command from the example directory:
// gcc -o example_brcmegl -s -O2 example_brcmegl.c -DNANOVG_GLES2_IMPLEMENTATION demo.c perf.c -I../src ../src/nanovg.c $(PKG_CONFIG_PATH=/opt/vc/lib/pkgconfig pkg-config --cflags --libs brcmegl) -lrt -lm

#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <assert.h>
#include <linux/input.h>
#include <bcm_host.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "nanovg.h"
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"
#include "demo.h"
#include "perf.h"

#define check() assert(glGetError() == 0)

#define TIMEDIFF(CURRENT, PREVIOUS) ((CURRENT.tv_sec - PREVIOUS.tv_sec) + (CURRENT.tv_nsec - PREVIOUS.tv_nsec) / 1e9)

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define LOG_MODULE_INIT(label) {printf("i: ");\
for(int LOG_MODULE_CTR = 0; LOG_MODULE_CTR < LOG_MODULE_LVL; LOG_MODULE_CTR++) printf(" ");\
printf(label);};

typedef struct {
  uint32_t screen_width;
  uint32_t screen_height;
  
  // OpenGL|ES objects
  EGLDisplay display;
  EGLSurface surface;
  EGLContext context;
} renderer_backend_state_s;

typedef struct {
  uint32_t width;
  uint32_t height;
  
  DemoData data;
  NVGcontext* vg;
  PerfGraph fps;
} ui_state_s;

typedef struct {
  // poition
  double movets;
  uint32_t x;
  uint32_t y;
  uint32_t px;
  uint32_t py;
  
  // button state
  uint32_t lb;
  double lbts;
  uint32_t rb;
  double rbts;
  uint32_t mb;
  double mbts;
  
  // system specific
  struct pollfd pfd;
} mouse_state_s;

typedef struct {
  // demo specific
  int blowup;
  int screenshot;
  int premult;
  
  // time specific
  uint32_t fnum;
  double t;
  double dt;
  
  // system specific
  struct timespec prevt;
  struct timespec curt;
  struct timespec startt;
} simulation_state_s;

/*static void key(GLFWwindow* window, int key, int scancode, int action, int mods)
{
  NVG_NOTUSED(scancode);
  NVG_NOTUSED(mods);
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    glfwSetWindowShouldClose(window, GL_TRUE);
  if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
    blowup = !blowup;
  if (key == GLFW_KEY_S && action == GLFW_PRESS)
    screenshot = 1;
  if (key == GLFW_KEY_P && action == GLFW_PRESS)
    premult = !premult;
}*/

const char *bit_rep[16] = {
    [ 0] = "0000", [ 1] = "0001", [ 2] = "0010", [ 3] = "0011",
    [ 4] = "0100", [ 5] = "0101", [ 6] = "0110", [ 7] = "0111",
    [ 8] = "1000", [ 9] = "1001", [10] = "1010", [11] = "1011",
    [12] = "1100", [13] = "1101", [14] = "1110", [15] = "1111",
};

void print_byte(signed char byte) {
    printf("[0x%.02x %s%sb %i]", byte, bit_rep[(unsigned char)byte >> 4], bit_rep[(unsigned char)byte & 0x0F], (int)byte);
}

#define LOG_MODULE_LVL 1
static void initEGL(renderer_backend_state_s *rbs) {
  int32_t success = 0;
  EGLBoolean result;
  EGLint num_config;

  static EGL_DISPMANX_WINDOW_T nativewindow;

  DISPMANX_ELEMENT_HANDLE_T dispman_element;
  DISPMANX_DISPLAY_HANDLE_T dispman_display;
  DISPMANX_UPDATE_HANDLE_T dispman_update;
  VC_RECT_T dst_rect;
  VC_RECT_T src_rect;

  static const EGLint attribute_list[] = {
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_STENCIL_SIZE, 8,
    EGL_DEPTH_SIZE, 0,
    EGL_MIN_SWAP_INTERVAL, 0,
    //EGL_MAX_SWAP_INTERVAL, 0,
    EGL_CONFORMANT, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };

  static const EGLint context_attributes[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };
  EGLConfig config;
  
  LOG_MODULE_INIT("bcm_host_init\n");
  bcm_host_init();

  // get an EGL display connection
  LOG_MODULE_INIT("eglGetDisplay\n");
  rbs->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  assert(rbs->display != EGL_NO_DISPLAY);
  check();

  // initialize the EGL display connection
  LOG_MODULE_INIT("eglInitialize\n");
  result = eglInitialize(rbs->display, NULL, NULL);
  assert(EGL_FALSE != result);
  check();

  // get an appropriate EGL frame buffer configuration
  LOG_MODULE_INIT("eglChooseConfig\n");
  result = eglChooseConfig(rbs->display, attribute_list, &config, 1, &num_config);
  assert(EGL_FALSE != result);
  check();

  // get an appropriate EGL frame buffer configuration
  LOG_MODULE_INIT("eglBindAPI\n");
  result = eglBindAPI(EGL_OPENGL_ES_API);
  assert(EGL_FALSE != result);
  check();

  // create an EGL rendering context
  LOG_MODULE_INIT("eglCreateContext\n");
  rbs->context = eglCreateContext(rbs->display, config, EGL_NO_CONTEXT, context_attributes);
  assert(rbs->context != EGL_NO_CONTEXT);
  check();

  // create an EGL window surface
  LOG_MODULE_INIT("graphics_get_display_size");
  success = graphics_get_display_size(0 /* LCD */, &rbs->screen_width, &rbs->screen_height);
  assert(success >= 0);
  printf(" -> resolution: %ix%i\n", rbs->screen_width, rbs->screen_height);

  dst_rect.x = 0;
  dst_rect.y = 0;
  dst_rect.width = rbs->screen_width;
  dst_rect.height = rbs->screen_height;

  src_rect.x = 0;
  src_rect.y = 0;
  src_rect.width = rbs->screen_width << 16;
  src_rect.height = rbs->screen_height << 16;        

  LOG_MODULE_INIT("vc_dispmanx_display_open\n");
  dispman_display = vc_dispmanx_display_open(0 /* LCD */);
  LOG_MODULE_INIT("vc_dispmanx_update_start\n");
  dispman_update = vc_dispmanx_update_start(0);

  LOG_MODULE_INIT("vc_dispmanx_element_add\n");
  dispman_element = vc_dispmanx_element_add(dispman_update, dispman_display, 0/*layer*/, &dst_rect, 0/*src*/, &src_rect, DISPMANX_PROTECTION_NONE, 0 /*alpha*/, 0/*clamp*/, 0/*transform*/);

  nativewindow.element = dispman_element;
  nativewindow.width = rbs->screen_width;
  nativewindow.height = rbs->screen_height;
  
  LOG_MODULE_INIT("vc_dispmanx_update_submit_sync\n");
  vc_dispmanx_update_submit_sync(dispman_update);
  check();

  LOG_MODULE_INIT("eglCreateWindowSurface\n");
  rbs->surface = eglCreateWindowSurface( rbs->display, config, &nativewindow, NULL );
  assert(rbs->surface != EGL_NO_SURFACE);
  check();

  // connect the context to the surface
  LOG_MODULE_INIT("eglMakeCurrent\n");
  result = eglMakeCurrent(rbs->display, rbs->surface, rbs->surface, rbs->context);
  assert(EGL_FALSE != result);
  check();
  
  LOG_MODULE_INIT("eglSwapInterval\n");
  eglSwapInterval(rbs->display, 0);

  // Set background color and clear buffers
  glClearColor(0.15f, 0.25f, 0.35f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  check();
}

static void initMouse(mouse_state_s *ms, ui_state_s *uis) {
  memset(ms, 0, sizeof(mouse_state_s));
  
  ms->px = ms->x = uis->width / 2;
  ms->py = ms->y = uis->height / 2;
  
  ms->pfd = (struct pollfd){.fd = -1, .events = POLLIN};
  const char *pDevice = "/dev/input/mice";
  
  // Open Mouse
  ms->pfd.fd = open(pDevice, O_RDWR | O_NOCTTY | O_NONBLOCK);

  if (ms->pfd.fd == -1) {
    fprintf(stderr, "m: unable to open input device: %s. mouse will be disabled.\n", pDevice);
  }
}

static void initUI(ui_state_s *uis, renderer_backend_state_s *rbs) {
  uis->width = rbs->screen_width;
  uis->height = rbs->screen_height;
  
  LOG_MODULE_INIT("initGraph\n");
  initGraph(&uis->fps, GRAPH_RENDER_FPS, "Frame Time");
  
  LOG_MODULE_INIT("nvgCreateGLES2\n");
  uis->vg = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
  assert(uis->vg != NULL);

  LOG_MODULE_INIT("loadDemoData\n");
  assert(loadDemoData(uis->vg, &uis->data) != -1);
}

static void initSimulation(simulation_state_s *ss) {
  memset(ss, 0, sizeof(simulation_state_s));
  
  clock_gettime(CLOCK_MONOTONIC, &ss->startt);
  ss->prevt = ss->startt;
}

static void updateSimulationTimers(simulation_state_s *ss) {
  clock_gettime(CLOCK_MONOTONIC, &ss->curt);
  ss->t = TIMEDIFF(ss->curt, ss->startt);
  ss->dt = TIMEDIFF(ss->curt, ss->prevt);
  ss->prevt = ss->curt;
}

// inspired by: https://stackoverflow.com/questions/52233626/mouse-event-handling-in-linux
static int handleLinuxMice(mouse_state_s *ms, ui_state_s *uis, simulation_state_s *ss) {
  if (ms->pfd.fd == -1) {
    return -1;
  }
  
  int newx = ms->px = ms->x;
  int newy = ms->py = ms->y;

  ms->pfd.revents = 0;
  int pres = poll(&ms->pfd, 1, 0);
  
  if (pres == -1 && errno != EINTR) {
    fprintf(stderr, "m: poll(): %s.\n", strerror(errno));
    
    return -1;
  }
  else if (pres > 0 && ms->pfd.revents & POLLIN) {
    do {
      signed char mice_data[3];
      int bytes = read(ms->pfd.fd, mice_data, sizeof(mice_data));
      
      if (bytes != sizeof(mice_data)) {
        if (bytes != -1) {
          printf("m: out of sync: %i\n", bytes);
        }
        else {
#ifdef MOUSE_LOGGING
          int prcx = ms->x, prcy = ms->y;
#endif
          ms->x = MAX(MIN(newx, (int)uis->width), 0);
          ms->y = MAX(MIN(newy, (int)uis->height), 0);
#ifdef MOUSE_LOGGING
          printf("m: [%i]%i(%i) [%i]%i(%i)\n", prcx, ms->x, ms->x - ms->px, prcy, ms->y, ms->y - ms->py);
#endif
        }
        
        break;
      }
      
      // invert y here to make better look of it in the logs
      mice_data[2] = -mice_data[2];
      
#ifdef MOUSE_LOGGING
      printf("m: ");
      for (int i = 0; i < sizeof(mice_data); i++) {
        print_byte(mice_data[i]);
        printf(i != sizeof(mice_data) - 1 ? " " : "\n");
      }
#endif
      
      newx += mice_data[1];
      newy += mice_data[2];
      
      if(ms->lb ^ (mice_data[0] & 0x01)) {
        ms->lb = (mice_data[0] & 0x01) ? 1 : 0;
        ms->lbts = ss->t;
      }
      
      if(ms->rb ^ (mice_data[0] & 0x02)) {
        ms->rb = (mice_data[0] & 0x02) ? 1 : 0;
        ms->rbts = ss->t;
      }
      
      if(ms->mb ^ (mice_data[0] & 0x04)) {
        ms->mb = (mice_data[0] & 0x04) ? 1 : 0;
        ms->mbts = ss->t;
      }
      
    } while (1);
    
    return 1;
  }

  return 0; 
}

void drawMouse(mouse_state_s *ms, ui_state_s *uis, simulation_state_s *ss) {
  int i;
  float r0, r1, aeps;
  float hue = sinf(ss->t * 0.12f) * 10.0;

  nvgSave(uis->vg);
  
  //nvgRotate(uis->vg, hue);

  r1 = (uis->width < uis->height ? uis->width : uis->height) * 0.05f - 5.0f;
  r0 = r1 - 20.0f;
  aeps = 0.5f / r1;  // half a pixel arc length in radians (2pi cancels out).
  
  nvgStrokeColor(uis->vg, nvgRGBA(255,255,255,128));
  nvgStrokeWidth(uis->vg, 3.0f);

  for (i = 0; i < 6; i++) {
    float a0 = (float)i / 6.0f * NVG_PI * 2.0f - aeps;
    float a1 = (float)(i+1.0f) / 6.0f * NVG_PI * 2.0f + aeps;
    nvgBeginPath(uis->vg);
    nvgArc(uis->vg, ms->x, ms->y, r0 + hue, a0, a1, NVG_CW);
    nvgArc(uis->vg, ms->x, ms->y, r1 - hue, a1, a0, NVG_CCW);
    nvgClosePath(uis->vg);
    nvgStroke(uis->vg);
  }

  nvgRestore(uis->vg);
}

#undef LOG_MODULE_LVL
#define LOG_MODULE_LVL 0
int main() {
  renderer_backend_state_s rbsr;
  renderer_backend_state_s *rbs = &rbsr;
  mouse_state_s msr;
  mouse_state_s *ms = &msr;
  simulation_state_s ssr;
  simulation_state_s *ss = &ssr;
  ui_state_s uisr;
  ui_state_s *uis = &uisr;
  
  LOG_MODULE_INIT("initEGL\n");
  initEGL(rbs);
  
  LOG_MODULE_INIT("initUI\n");
  initUI(uis, rbs);
  
  LOG_MODULE_INIT("initMouse\n");
  initMouse(ms, uis);

  LOG_MODULE_INIT("initSimulation\n");
  initSimulation(ss);

  while (1) {
    updateSimulationTimers(ss);
    updateGraph(&uis->fps, ss->dt);
    handleLinuxMice(ms, uis, ss);

    // Update and render
    glViewport(0, 0, rbs->screen_width, rbs->screen_height);
    
    if (ss->premult) {
      glClearColor(0,0,0,0);
    }
    else {
      glClearColor(0.3f, 0.3f, 0.32f, 1.0f);
    }
    
    glClear(GL_COLOR_BUFFER_BIT/*|GL_DEPTH_BUFFER_BIT*/|GL_STENCIL_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    //glDisable(GL_DEPTH_TEST);

    nvgBeginFrame(uis->vg, uis->width, uis->height, 1);

    renderDemo(uis->vg, ms->x, ms->y, uis->width, uis->height, ss->t, ss->blowup, &uis->data);
    renderGraph(uis->vg, 5, 5, &uis->fps);
    drawMouse(ms, uis, ss);

    nvgEndFrame(uis->vg);

    if (ss->screenshot) {
      ss->screenshot = 0;
      saveScreenShot(rbs->screen_width, rbs->screen_height, ss->premult, "dump.png");
    }
    
    //glEnable(GL_DEPTH_TEST);
    
    glFlush();
    glFinish();
    check();

    eglSwapBuffers(rbs->display, rbs->surface);
    check();
    
    ss->fnum++;
  }

  LOG_MODULE_INIT("freeDemoData\n");
  freeDemoData(uis->vg, &uis->data);

  LOG_MODULE_INIT("nvgDeleteGLES2\n");
  nvgDeleteGLES2(uis->vg);
  
  printf("Statistics: %i frames in %.2f secs = %.1f FPS\n", ss->fnum, ss->t, ss->fnum / ss->t);

  return 0;
}
