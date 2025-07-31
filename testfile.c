// 文件名: camera_fb.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h> // for exit
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <jpeglib.h> // For JPEG decoding

/* --- 全局变量，用于Framebuffer和V4L2 --- */
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
char *fbp = 0;
long int screensize = 0;
int fbfd = 0;

int v4l2_fd = -1;
int buffer_count;
void *v4l2_bufs[4];
char filename[32];
int file_cnt = 0;

/* --- 函数声明 --- */
static void fb_init(const char *fb_device);
static void fb_draw_buffer(unsigned char *jpeg_buffer, unsigned long jpeg_size);
static void cleanup();

/**
 * @brief 初始化 Framebuffer
 * @param fb_device Framebuffer设备路径，如 /dev/fb0
 */
static void fb_init(const char *fb_device) {
    fbfd = open(fb_device, O_RDWR);
    if (fbfd == -1) {
        perror("Error: cannot open framebuffer device");
        exit(1);
    }

    // 获取屏幕可变信息
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {
        perror("Error reading variable information");
        exit(3);
    }
    printf("Framebuffer: %dx%d, %d bpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    // 获取屏幕固定信息
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) {
        perror("Error reading fixed information");
        exit(2);
    }

    screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;

    // 映射 Framebuffer 到内存
    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if ((intptr_t)fbp == -1) {
        perror("Error: failed to map framebuffer device to memory");
        exit(4);
    }
}

/**
 * @brief 将一帧 MJPEG 图像解码并绘制到 Framebuffer
 * @param jpeg_buffer 包含JPEG数据的缓冲区指针
 * @param jpeg_size JPEG数据的大小
 */
static void fb_draw_buffer(unsigned char *jpeg_buffer, unsigned long jpeg_size) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    
    JSAMPARRAY buffer; // 输出行缓冲区
    int row_stride;    // 物理行宽

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_buffer, jpeg_size); // 从内存中读取jpeg数据

    // 读取jpeg文件头
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        fprintf(stderr, "jpeg_read_header failed\n");
        jpeg_destroy_decompress(&cinfo);
        return;
    }

    // 设置解压参数，比如放大、缩小
    cinfo.out_color_space = JCS_RGB; // 设置输出格式为RGB
    jpeg_start_decompress(&cinfo);

    row_stride = cinfo.output_width * cinfo.output_components;
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);
    
    // 计算绘制的起始位置（屏幕中心）
    long int location = 0;
    int x_start = (vinfo.xres - cinfo.output_width) / 2;
    int y_start = (vinfo.yres - cinfo.output_height) / 2;
    if (x_start < 0) x_start = 0;
    if (y_start < 0) y_start = 0;

    // 逐行读取并绘制
    while (cinfo.output_scanline < cinfo.output_height) {
        int y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, buffer, 1);

        // 如果该行在屏幕范围内
        if ((y + y_start) < vinfo.yres) {
            location = (x_start * (vinfo.bits_per_pixel / 8)) + 
                       ((y + y_start) * finfo.line_length);
            
            // 将解码后的RGB数据复制到framebuffer内存
            int x;
            for (x = 0; x < cinfo.output_width; x++) {
                // 如果该点在屏幕范围内
                if ((x + x_start) < vinfo.xres) {
                    long int pixel_location = location + (x * (vinfo.bits_per_pixel / 8));
                    // 假设是 32bpp (ARGB) 或 24bpp (RGB)
                    if (vinfo.bits_per_pixel == 32) {
                        *(fbp + pixel_location) = buffer[0][x * 3 + 2];     // Blue
                        *(fbp + pixel_location + 1) = buffer[0][x * 3 + 1]; // Green
                        *(fbp + pixel_location + 2) = buffer[0][x * 3 + 0]; // Red
                        *(fbp + pixel_location + 3) = 0;                    // Alpha
                    } else if (vinfo.bits_per_pixel == 24) { // 24bpp
                        *(fbp + pixel_location) = buffer[0][x * 3 + 2];     // Blue
                        *(fbp + pixel_location + 1) = buffer[0][x * 3 + 1]; // Green
                        *(fbp + pixel_location + 2) = buffer[0][x * 3 + 0]; // Red
                    }
                    // TODO: 可以添加对 16bpp (RGB565) 的支持
                }
            }
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
}

/**
 * @brief 清理所有资源
 */
static void cleanup() {
    if (v4l2_fd != -1) {
        ioctl(v4l2_fd, VIDIOC_STREAMOFF, &(int){V4L2_BUF_TYPE_VIDEO_CAPTURE});
        printf("V4L2 stream stopped.\n");

        for(int i = 0; i < buffer_count; i++) {
            if (v4l2_bufs[i]) {
                struct v4l2_buffer buf_info;
                memset(&buf_info, 0, sizeof(buf_info));
                buf_info.index = i;
                buf_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf_info.memory = V4L2_MEMORY_MMAP;
                if(ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf_info) == 0) {
                     munmap(v4l2_bufs[i], buf_info.length);
                }
            }
        }
        printf("V4L2 buffers unmapped.\n");
        close(v4l2_fd);
    }

    if (fbp) {
        munmap(fbp, screensize);
        printf("Framebuffer unmapped.\n");
    }
    if (fbfd > 0) {
        close(fbfd);
    }
}


int main(int argc, char **argv)
{ 
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	
	if(argc!=3){
		printf("Usage: %s </dev/videoX> </dev/fbY>\n", argv[0]);
        return -1;
	}
    
    // 注册清理函数，以便在程序退出时（包括Ctrl+C）调用
    atexit(cleanup);
    
    // --- 1. 初始化 Framebuffer ---
    fb_init(argv[2]);

	// --- 2. 初始化 V4L2 ---
	v4l2_fd = open(argv[1], O_RDWR);
	if(v4l2_fd < 0){
		perror("Can't open video device");
		exit(1);
	}

	// 查询能力
	struct v4l2_capability cap;
	if(ioctl(v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0){
		perror("VIDIOC_QUERYCAP");
		exit(1);
	}
	if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support video capture or streaming.\n");
        exit(1);
    }

	// 设置格式
	struct v4l2_format fmt;	
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = 640;  // 使用一个较小的分辨率以提高帧率
	fmt.fmt.pix.height = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;
	if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0){
		perror("VIDIOC_S_FMT");
        // 如果640x480不支持，尝试其他分辨率
        fmt.fmt.pix.width = 1280;
        fmt.fmt.pix.height = 1024;
        if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT 1280x1024 also failed");
            exit(1);
        }
	}
    printf("V4L2 format set to: %dx%d MJPEG\n", fmt.fmt.pix.width, fmt.fmt.pix.height);

	// 申请buffer
	struct v4l2_requestbuffers reqbuffer;
	memset(&reqbuffer, 0, sizeof(reqbuffer));
	reqbuffer.count = 4;
	reqbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuffer.memory = V4L2_MEMORY_MMAP;
    if(ioctl(v4l2_fd, VIDIOC_REQBUFS, &reqbuffer) < 0) {
        perror("VIDIOC_REQBUFS");
        exit(1);
    }
	buffer_count = reqbuffer.count;

	// 映射和入队buffer
	for(int i=0; i<buffer_count; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.index = i;
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF"); exit(1);
        }
		v4l2_bufs[i] = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.offset);
		if(v4l2_bufs[i] == MAP_FAILED){
            perror("mmap"); exit(1);
        }
		if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF"); exit(1);
        }
	}

	// 启动摄像头
	if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON"); exit(1);
    }
	printf("Camera stream started.\n");

    // --- 3. 主循环 ---
    printf("Press [Enter] to capture an image. Press Ctrl+C to exit.\n");
	struct pollfd fds[2];
	while(1){ 
        fds[0].fd = v4l2_fd;
        fds[0].events = POLLIN;	
        fds[1].fd = STDIN_FILENO;
        fds[1].events = POLLIN;

		int ret = poll(fds, 2, 2000); // 2秒超时
        if (ret < 0) {
            perror("poll error");
            break;
        }
        if (ret == 0) {
            printf("poll timeout\n");
            continue;
        }
        
        // 处理键盘输入
        if (fds[1].revents & POLLIN) {
            if (getchar() == '\n') { // 按下回车键
                printf("Capture command received! Saving next valid frame...\n");
                
                // 为了保证拍到的是新的一帧，我们这里直接等待下一帧摄像头数据
                struct pollfd pfd_cap;
                pfd_cap.fd = v4l2_fd;
                pfd_cap.events = POLLIN;
                if(poll(&pfd_cap, 1, 1000) > 0) {
                    struct v4l2_buffer buf;
			        memset(&buf, 0, sizeof(buf));
			        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			        buf.memory = V4L2_MEMORY_MMAP;
			        if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) == 0) {
                        // 保存文件
                        sprintf(filename,"capture_%04d.jpg",file_cnt++);         
			            int fd_out = open(filename, O_CREAT | O_RDWR | O_TRUNC, 0666);
                        if(fd_out > 0) {
                            write(fd_out, v4l2_bufs[buf.index], buf.bytesused);
                            close(fd_out);
                            printf("Saved to %s\n", filename);
                        }
                        // 重新入队
                        ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
                    }
                }
            }
        }
		
        // 处理摄像头数据
		if (fds[0].revents & POLLIN) { 
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) == 0) {
                // 实时显示到Framebuffer
                fb_draw_buffer(v4l2_bufs[buf.index], buf.bytesused);
                
                // 重新入队
			    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
				    perror("VIDIOC_QBUF in loop");
				    break;
			    }
            } else {
                perror("VIDIOC_DQBUF");
            }
		}
	}

	return 0; // cleanup() 会被自动调用
}