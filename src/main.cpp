#include <SDL2/SDL.h>
#include "spdlog/spdlog.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

#define ERROR_GENERAL -1

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

SDL_Window *window;
SDL_Renderer *renderer;
SDL_Texture *cameraTexture;

int init() {
    int rc = SDL_Init(SDL_INIT_VIDEO);
    if(rc) {
        spdlog::critical("failed to initialize sdl2 subsystem, rc: %d", rc);
        return ERROR_GENERAL;
    }
    spdlog::info("SDL2 initialized");

    window = SDL_CreateWindow("V4L2 Camera", 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL);
    if(!window) {
        spdlog::critical("failed to create a window");
        return ERROR_GENERAL;
    }
    spdlog::info("window is created");

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if(!renderer) {
        spdlog::critical("failed to create a renderer");
        return ERROR_GENERAL;
    }
    spdlog::info("renderer is created");

    cameraTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);
    if(!cameraTexture) {
        spdlog::critical("failed to create a texture");
        return ERROR_GENERAL;
    }
    spdlog::info("texture is created");

    return 0;
}

#define DEVICE_NAME "/dev/video0"
#define CAMERA_WIDTH 1280
#define CAMERA_HEIGHT 720
#define FPS 30
int cameraFD = -1;
void *cameraBuffer;
struct v4l2_buffer cameraBufferInfo;

int initV4L2() {
    spdlog::info("Opening {}", DEVICE_NAME);

    cameraFD = open(DEVICE_NAME, O_RDWR);
    if(cameraFD < 0) {
        spdlog::critical("failed to open video device, fd: {}", cameraFD);
        return ERROR_GENERAL;
    }

    // Get camera info
    {
        struct v4l2_capability cap;
        int rc = ioctl(cameraFD, VIDIOC_QUERYCAP, &cap);
        if(rc < 0) {
            spdlog::critical("failed to query capabilities, rc: {}", rc);
            return ERROR_GENERAL;
        }

        spdlog::info("Got camera!");
        spdlog::info("Driver: {}", std::string(reinterpret_cast<char *>(cap.driver)));
        spdlog::info("Card: {}", std::string(reinterpret_cast<char *>(cap.card)));
        spdlog::info("Info: {}", std::string(reinterpret_cast<char *>(cap.bus_info)));
    }

    // Set pixel format
    {
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = CAMERA_WIDTH;
        fmt.fmt.pix.height = CAMERA_HEIGHT;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.colorspace = V4L2_COLORSPACE_DEFAULT;

        int rc = ioctl(cameraFD, VIDIOC_S_FMT, &fmt);
        if(rc < 0) {
            spdlog::critical("failed to set pixel format, rc: {}", rc);
            return ERROR_GENERAL;
        }

        spdlog::info("pixelformat setted up");
    }

    // Set the frame rate
    {
        struct v4l2_streamparm streamparm;
        memset(&streamparm, 0, sizeof(streamparm));
        streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        streamparm.parm.capture.timeperframe.numerator = 1; // 1 second
        streamparm.parm.capture.timeperframe.denominator = FPS; // 30 frames

        int rc = ioctl(cameraFD, VIDIOC_S_PARM, &streamparm);
        if(rc < 0) {
            spdlog::critical("failed to set framerate, rc: {}", rc);
            return ERROR_GENERAL;
        }

        spdlog::info("framerate setted up");
    }

    // Request buffers
    {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = 1; // Request one buffer
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        int rc = ioctl(cameraFD, VIDIOC_REQBUFS, &req);
        if(rc < 0) {
            spdlog::critical("failed to request buffer, rc: {}", rc);
            return ERROR_GENERAL;
        }

        spdlog::info("got {} buffers", req.count);
    }

    // Map the buffer
    {
        cameraBufferInfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cameraBufferInfo.memory = V4L2_MEMORY_MMAP;
        cameraBufferInfo.index = 0;

        int rc = ioctl(cameraFD, VIDIOC_QUERYBUF, &cameraBufferInfo);
        if(rc < 0) {
            spdlog::critical("failed to map the buffer, rc: {}", rc);
            return ERROR_GENERAL;
        }
        spdlog::info("mapped {} buffer, length: {}", cameraBufferInfo.index, cameraBufferInfo.length);

        cameraBuffer = mmap(NULL, cameraBufferInfo.length,
            PROT_READ | PROT_WRITE, MAP_SHARED, cameraFD, cameraBufferInfo.m.offset);
        if(cameraBuffer == MAP_FAILED) {
            spdlog::critical("failed to get the pointer of the buffer, rc: {}", cameraBuffer);
            return ERROR_GENERAL;
        }
        spdlog::info("got buffer pointer, {}", cameraBuffer);
    }

    //cameraBuffer = malloc(CAMERA_WIDTH * CAMERA_HEIGHT * 3);
    return 0;
}

void closeCamera() {
    spdlog::info("closing camera");

    if((MAP_FAILED != cameraBuffer) && (cameraBuffer)) {
        munmap(cameraBuffer, cameraBufferInfo.length);
    }
    if(cameraFD >= 0) {
        close(cameraFD);
    }
}

void closeSDL() {
    spdlog::info("closing sdl");

    if(cameraTexture) {
        SDL_DestroyTexture(cameraTexture);
    }
    if(renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if(window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

int stream() {
    SDL_Event event;

    while(true) {
        SDL_PollEvent(&event);
        if(event.type == SDL_QUIT) {
            spdlog::info("Exit called");
            return 0;
        }
        SDL_PumpEvents();

        // Get a buffer
        int rc = ioctl(cameraFD, VIDIOC_DQBUF, &cameraBufferInfo);
        if(rc < 0) {
            spdlog::critical("failed to get a frame, rc: {}", rc);
            return rc;
        }

        // Show buffer
        {
            SDL_UpdateTexture(cameraTexture, NULL, cameraBuffer, CAMERA_WIDTH * 2);
            SDL_RenderCopy(renderer, cameraTexture, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        // Get new the buffer
        rc = ioctl(cameraFD, VIDIOC_QBUF, &cameraBufferInfo);
        if(rc < 0) {
            spdlog::critical("failed to release a frame, rc: {}", rc);
            return rc;
        }

        // Wait for the next frame (33.33 ms)
        usleep(1000000 / FPS);
    }
}

int main() {
    spdlog::set_pattern("[%m/%d %H:%M:%S] %v");
    spdlog::info("v4l2_camera is launching");

    int rc = init();
    if(rc) {
        spdlog::critical("failed to initialize an app, rc: {}", rc);
        exit(EXIT_FAILURE);
    }
    spdlog::info("initialized");

    rc = initV4L2();
    if(rc) {
        spdlog::critical("failed to initialize a camera, rc: {}", rc);

        closeCamera();
        closeSDL();
        exit(EXIT_FAILURE);
    }

    /*cameraBufferInfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cameraBufferInfo.memory = V4L2_MEMORY_USERPTR;
    cameraBufferInfo.length = CAMERA_WIDTH * CAMERA_HEIGHT * 3;
    cameraBufferInfo.m.userptr = reinterpret_cast<unsigned long>(cameraBuffer);*/

    // get first frame
    rc = ioctl(cameraFD, VIDIOC_QBUF, &cameraBufferInfo);
    if(rc < 0) {
        spdlog::critical("failed to get first frame, rc: {}, {}", rc, strerror(errno));

        closeCamera();
        closeSDL();
        return EXIT_FAILURE;
    }

    // start stream
    rc = ioctl(cameraFD, VIDIOC_STREAMON, &cameraBufferInfo.type);
    if(rc < 0) {
        spdlog::critical("failed to start a stream, rc: {}", rc);

        closeCamera();
        closeSDL();
        return EXIT_FAILURE;
    }

    // steam
    rc = stream();
    spdlog::critical(rc ? "failed to stop a loop, rc: {}" : "loop stopped, rc: {}", rc);

    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, cameraTexture, NULL, NULL);
    SDL_RenderPresent(renderer);

    // stop stream
    rc = ioctl(cameraFD, VIDIOC_STREAMOFF, &cameraBufferInfo.type);
    spdlog::critical(rc ? "failed to stop a stream, rc: {}" : "stream stopped, rc: {}", rc);

    closeCamera();
    closeSDL();

    spdlog::info("v4l2_camera is exiting");
    return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}