// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/typedesc.h>
#if (OIIO_VERSION < 10100)
namespace OIIO = OIIO_NAMESPACE;
#endif


#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <GLUT/glut.h>
#elif _WIN32
#include <GL/glew.h>
#include <GL/glut.h>
#else
#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glut.h>
#endif

#include "glsl.h"
#include "OpenEXR/half.h"
#include "oiiohelpers.h"


#include "apputils/argparse.h"

// Array of non OpenColorIO arguments.
static std::vector<std::string> args;


// Fill 'args' array with OpenColorIO arguments.
static int
parse_end_args(int argc, const char *argv[])
{
  while(argc>0)
  {
    args.push_back(argv[0]);
    argc--;
    argv++;
  }

  return 0;
}

class GPUManagement
{
private:
    GPUManagement()
        : m_glwin(0)
        , m_initState(STATE_CREATED)
        , m_imageTexID(0)
        , m_format(0)
        , m_width(0)
        , m_height(0)
    {
    }

    GPUManagement(const GPUManagement &) = delete;
    GPUManagement & operator=(const GPUManagement &) = delete;

    ~GPUManagement()
    {
        if (m_initState != STATE_CREATED)
        {
            cleanUp();
        }
    }

public:

    static GPUManagement & Instance()
    {
        static GPUManagement inst;
        return inst;
    }

    void init(bool verbose)
    {
        if (m_initState != STATE_CREATED) return;

        int argcgl = 2;
        const char* argvgl[] = { "main", "-glDebug" };
        glutInit(&argcgl, const_cast<char**>(&argvgl[0]));

        glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
        glutInitWindowSize(10, 10);
        glutInitWindowPosition(0, 0);

        m_glwin = glutCreateWindow(argvgl[0]);

#ifndef __APPLE__
        glewInit();
        if (!glewIsSupported("GL_VERSION_2_0"))
        {
            std::cerr << "OpenGL 2.0 not supported" << std::endl;
            exit(1);
        }
#endif

        if(verbose)
        {
            std::cout << std::endl
                      << "GL Vendor:    " << glGetString(GL_VENDOR) << std::endl
                      << "GL Renderer:  " << glGetString(GL_RENDERER) << std::endl
                      << "GL Version:   " << glGetString(GL_VERSION) << std::endl
                      << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
        }

        // Initialize the OpenGL engine
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);           // 4-byte pixel alignment
#ifndef __APPLE__
        glClampColor(GL_CLAMP_READ_COLOR, GL_FALSE);     //
        glClampColor(GL_CLAMP_VERTEX_COLOR, GL_FALSE);   // avoid any kind of clamping
        glClampColor(GL_CLAMP_FRAGMENT_COLOR, GL_FALSE); //
#endif

        glEnable(GL_TEXTURE_2D);
        glClearColor(0, 0, 0, 0);                        // background color
        glClearStencil(0);                               // clear stencil buffer

        m_initState = STATE_INITIALIZED;
    }

    void prepareImage(float * data, long width, long height, long numChannels)
    {
        if (m_initState != STATE_INITIALIZED)
        {
            std::cerr << "The GPU engine is not initialized." << std::endl;
            exit(1);
        }

        m_width = width;
        m_height = height;

        if (numChannels == 4) m_format = GL_RGBA;
        else if (numChannels == 3) m_format = GL_RGB;
        else
        {
            std::cerr << "Cannot process with GPU image with "
                << numChannels << " components." << std::endl;
            exit(1);
        }

        glGenTextures(1, &m_imageTexID);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_imageTexID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F_ARB, width, height, 0,
            m_format, GL_FLOAT, &data[0]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

        // Create the frame buffer and render buffer
        GLuint fboId;

        // create a framebuffer object, you need to delete them when program exits.
        glGenFramebuffers(1, &fboId);
        glBindFramebuffer(GL_FRAMEBUFFER, fboId);

        GLuint rboId;
        // create a renderbuffer object to store depth info
        glGenRenderbuffers(1, &rboId);
        glBindRenderbuffer(GL_RENDERBUFFER, rboId);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F_ARB, m_width, m_height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        // attach a texture to FBO color attachment point
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_imageTexID, 0);

        // attach a renderbuffer to depth attachment point
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboId);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Set the rendering destination to FBO
        glBindFramebuffer(GL_FRAMEBUFFER, fboId);

        // Clear buffer
        glClearColor(0.1f, 0.1f, 0.1f, 0.1f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        m_initState = STATE_IMAGE_PREPARED;
    }

    void updateGPUShader(
        const OCIO::ConstProcessorRcPtr & processor, bool legacyShader, bool gpuinfo)
    {
        if (m_initState != STATE_IMAGE_PREPARED)
        {
            std::cerr << "GPU image not prepared." << std::endl;
            exit(1);
        }

        OCIO::GpuShaderDescRcPtr shaderDesc
            = legacyShader ? OCIO::GpuShaderDesc::CreateLegacyShaderDesc(32)
                           : OCIO::GpuShaderDesc::CreateShaderDesc();

        // Create the GPU shader description
        shaderDesc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_3);

        // Collect the shader program information for a specific processor    
        OCIO::ConstGPUProcessorRcPtr gpuProcessor
            = processor->getDefaultGPUProcessor();
        gpuProcessor->extractGpuShaderInfo(shaderDesc);

        // Use the helper OpenGL builder
        m_oglBuilder = OCIO::OpenGLBuilder::Create(shaderDesc);
        m_oglBuilder->setVerbose(gpuinfo);

        // Allocate & upload all the LUTs
        m_oglBuilder->allocateAllTextures(1);

        std::ostringstream main;
        main << std::endl
            << "uniform sampler2D img;" << std::endl
            << std::endl
            << "void main()" << std::endl
            << "{" << std::endl
            << "    vec4 col = texture2D(img, gl_TexCoord[0].st);" << std::endl
            << "    gl_FragColor = " << shaderDesc->getFunctionName() << "(col);" << std::endl
            << "}" << std::endl;

        // Build the fragment shader program
        m_oglBuilder->buildProgram(main.str().c_str());

        // Enable the fragment shader program, and all needed textures
        m_oglBuilder->useProgram();
        // The image texture
        glUniform1i(glGetUniformLocation(m_oglBuilder->getProgramHandle(), "img"), 0);
        // The LUT textures
        m_oglBuilder->useAllTextures();
        // Enable uniforms for dynamic properties
        m_oglBuilder->useAllUniforms();

        m_initState = STATE_SHADER_UPDATED;
    }

    void processImage()
    {
        if (m_initState != STATE_SHADER_UPDATED)
        {
            std::cerr << "GPU shader has not been updated." << std::endl;
            exit(1);
        }

        glViewport(0, 0, m_width, m_height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, m_width, 0.0, m_height, -100.0, 100.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glEnable(GL_TEXTURE_2D);
        glClearColor(0.1f, 0.1f, 0.1f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1, 1, 1);
        glPushMatrix();
            glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 1.0f);
                glVertex2f(0.0f, (float)m_height);

                glTexCoord2f(0.0f, 0.0f);
                glVertex2f(0.0f, 0.0f);

                glTexCoord2f(1.0f, 0.0f);
                glVertex2f((float)m_width, 0.0f);

                glTexCoord2f(1.0f, 1.0f);
                glVertex2f((float)m_width, (float)m_height);
            glEnd();
        glPopMatrix();
        glDisable(GL_TEXTURE_2D);

        glutSwapBuffers();

        m_initState = STATE_IMAGE_PROCESSED;
    }

    void readImage(const float * image)
    {
        if (m_initState != STATE_IMAGE_PROCESSED)
        {
            std::cerr << "Image has not been processed by GPU shader." << std::endl;
            exit(1);
        }

        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, m_width, m_height, m_format, GL_FLOAT, (GLvoid*)image);

        // Current implementation only has to process 1 image.
        // To handle more images we could go back to STATE_INITIALIZED
        m_initState = STATE_IMAGE_READ;
    }

private:
    void cleanUp()
    {
        m_oglBuilder.reset();
        glutDestroyWindow(m_glwin);
        m_initState = STATE_CREATED;
    }

    enum State
    {
        STATE_CREATED,
        STATE_INITIALIZED,
        STATE_IMAGE_PREPARED,
        STATE_SHADER_UPDATED,
        STATE_IMAGE_PROCESSED,
        STATE_IMAGE_READ
    };

    GLint m_glwin;
    State m_initState;
    OCIO::OpenGLBuilderRcPtr m_oglBuilder;
    GLuint m_imageTexID;
    GLenum m_format;
    long m_width;
    long m_height;
};

bool ParseNameValuePair(std::string& name, std::string& value,
                        const std::string& input);

bool StringToFloat(float * fval, const char * str);

bool StringToInt(int * ival, const char * str);

bool StringToVector(std::vector<int> * ivector, const char * str);

int main(int argc, const char **argv)
{
    ArgParse ap;

    std::vector<std::string> floatAttrs;
    std::vector<std::string> intAttrs;
    std::vector<std::string> stringAttrs;
    std::string keepChannels;
    bool croptofull = false;
    bool usegpu = false;
    bool usegpuLegacy = false;
    bool outputgpuInfo = false;
    bool verbose = false;
    bool useLut = false;
    bool useDisplayView = false;

    ap.options("ocioconvert -- apply colorspace transform to an image \n\n"
               "usage: ocioconvert [options]  inputimage inputcolorspace outputimage outputcolorspace\n"
               "   or: ocioconvert [options] --lut lutfile inputimage outputimage\n"
               "   or: ocioconvert [options] --view inputimage inputcolorspace outputimage displayname viewname\n\n",
               "%*", parse_end_args, "",
               "<SEPARATOR>", "Options:",
               "--lut",       &useLut,         "Convert using a LUT rather than a config file",
               "--view",      &useDisplayView, "Convert to a (display,view) pair rather than to "
                                               "an output color space",
               "--gpu",       &usegpu,         "Use GPU color processing instead of CPU (CPU is the default)",
               "--gpulegacy", &usegpuLegacy,   "Use the legacy (i.e. baked) GPU color processing "
                                               "instead of the CPU one (--gpu is ignored)",
               "--gpuinfo",  &outputgpuInfo,   "Output the OCIO shader program",
               "--v",        &verbose,         "Display general information",
               "<SEPARATOR>", "\nOpenImageIO options:",
               "--float-attribute %L",  &floatAttrs,   "\"name=float\" pair defining OIIO float attribute "
                                                       "for outputimage",
               "--int-attribute %L",    &intAttrs,     "\"name=int\" pair defining OIIO int attribute "
                                                       "for outputimage",
               "--string-attribute %L", &stringAttrs,  "\"name=string\" pair defining OIIO string attribute "
                                                       "for outputimage",
               "--croptofull",          &croptofull,   "Crop or pad to make pixel data region match the "
                                                       "\"full\" region",
               "--ch %s",               &keepChannels, "Select channels (e.g., \"2,3,4\")",
               NULL
               );
    if (ap.parse (argc, argv) < 0)
    {
        std::cerr << ap.geterror() << std::endl;
        ap.usage ();
        exit(1);
    }

    const char * inputimage       = nullptr;
    const char * inputcolorspace  = nullptr;
    const char * outputimage      = nullptr;
    const char * outputcolorspace = nullptr;
    const char * lutFile          = nullptr;
    const char * display          = nullptr;
    const char * view             = nullptr;

    if (!useLut && !useDisplayView)
    {
        if (args.size() != 4)
        {
            std::cerr << "ERROR: Expecting 4 arguments, found " << args.size() << std::endl;
            ap.usage();
            exit(1);
        }
        inputimage       = args[0].c_str();
        inputcolorspace  = args[1].c_str();
        outputimage      = args[2].c_str();
        outputcolorspace = args[3].c_str();
    }
    else if (useLut && useDisplayView)
    {
        std::cerr << "ERROR: Options lut & view can't be used at the same time." << std::endl;
        ap.usage();
        exit(1);
    }
    else if (useLut)
    {
        if (args.size() != 3)
        {
            std::cerr << "ERROR: Expecting 3 arguments for --lut option, found "
                      << args.size() << std::endl;
            ap.usage();
            exit(1);
        }
        lutFile     = args[0].c_str();
        inputimage  = args[1].c_str();
        outputimage = args[2].c_str();
    }
    else if (useDisplayView)
    {
        if (args.size() != 5)
        {
            std::cerr << "ERROR: Expecting 5 arguments for --view option, found "
                      << args.size() << std::endl;
            ap.usage();
            exit(1);
        }
        inputimage      = args[0].c_str();
        inputcolorspace = args[1].c_str();
        outputimage     = args[2].c_str();
        display         = args[3].c_str();
        view            = args[4].c_str();
    }

    if(verbose)
    {
        std::cout << std::endl;
        std::cout << "OIIO Version: " << OIIO_VERSION_STRING << std::endl;
        std::cout << "OCIO Version: " << OCIO::GetVersion() << std::endl;
        const char * env = getenv("OCIO");
        if(env && *env)
        {
            try
            {
                std::cout << std::endl;
                std::cout << "OCIO Configuration: '" << env << "'" << std::endl;
                OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
                std::cout << "OCIO search_path:    " << config->getSearchPath() << std::endl;
            }
            catch (const OCIO::Exception & e)
            {
                std::cout << "ERROR loading config file: " << e.what() << std::endl;
                exit(1);
            }
            catch(...)
            {

                std::cerr << "ERROR loading the config file: '" << env << "'";
                exit(1);
            }
        }
    }

    if (usegpuLegacy)
    {
        std::cout << std::endl;
        std::cout << "Using legacy OCIO v1 GPU color processing." << std::endl;
    }
    else if (usegpu)
    {
        std::cout << std::endl;
        std::cout << "Using GPU color processing." << std::endl;
    }

    OIIO::ImageSpec spec;
    OCIO::ImgBuffer img;
    int imgwidth = 0;
    int imgheight = 0;
    int components = 0;

    // Load the image
    std::cout << std::endl;
    std::cout << "Loading " << inputimage << std::endl;
    try
    {
#if OIIO_VERSION < 10903
        OIIO::ImageInput* f = OIIO::ImageInput::create(inputimage);
#else
        auto f = OIIO::ImageInput::create(inputimage);
#endif
        if(!f)
        {
            std::cerr << "ERROR: Could not create image input." << std::endl;
            exit(1);
        }

        f->open(inputimage, spec);

        std::string error = f->geterror();
        if(!error.empty())
        {
            std::cerr << "ERROR: Could not load image: " << error << std::endl;
            exit(1);
        }

        OCIO::PrintImageSpec(spec, verbose);

        imgwidth = spec.width;
        imgheight = spec.height;
        components = spec.nchannels;

        if (usegpu || usegpuLegacy)
        {
            spec.format = OIIO::TypeDesc::FLOAT;
            img.allocate(spec);

            const bool ok = f->read_image(spec.format, img.getBuffer());
            if(!ok)
            {
                std::cerr << "ERROR: Reading \"" << inputimage << "\" failed with: "
                          << f->geterror() << std::endl;
                exit(1);
            }

            if(croptofull)
            {
                std::cerr << "ERROR: Crop disabled in GPU mode" << std::endl;
                exit(1);
            }
        }
        else
        {
            img.allocate(spec);

            const bool ok = f->read_image(spec.format, img.getBuffer());
            if(!ok)
            {
                std::cerr << "ERROR: Reading \"" << inputimage << "\" failed with: "
                          << f->geterror() << std::endl;
                exit(1);
            }
        }

#if OIIO_VERSION < 10903
        OIIO::ImageInput::destroy(f);
#endif

        std::vector<int> kchannels;
        //parse --ch argument
        if (keepChannels != "" && !StringToVector(&kchannels, keepChannels.c_str()))
        {
            std::cerr << "ERROR: --ch: '" << keepChannels
                      << "' should be comma-seperated integers" << std::endl;
            exit(1);
        }

        //if kchannels not specified, then keep all channels
        if (kchannels.size() == 0)
        {
            kchannels.resize(components);
            for (int channel=0; channel < components; channel++)
            {
                kchannels[channel] = channel;
            }
        }

        if (croptofull)
        {
            imgwidth = spec.full_width;
            imgheight = spec.full_height;

            std::cout << "cropping to " << imgwidth
                      << "x" << imgheight << std::endl;
        }

        if (croptofull || (int)kchannels.size() < spec.nchannels)
        {
            // Redefine the spec so it matches the new bounding box.
            OIIO::ImageSpec croppedSpec = spec;

            croppedSpec.x = 0;
            croppedSpec.y = 0;
            croppedSpec.height    = imgheight;
            croppedSpec.width     = imgwidth;
            croppedSpec.nchannels = (int)(kchannels.size());

            OCIO::ImgBuffer croppedImg(croppedSpec);

            void * croppedBuf = croppedImg.getBuffer();
            void * imgBuf     = img.getBuffer();

            // crop down bounding box and ditch all but n channels
            // img is a flattened 3 dimensional matrix heightxwidthxchannels
            // fill croppedimg with only the needed pixels
            for (int y=0 ; y < spec.height ; y++)
            {
                for (int x=0 ; x < spec.width; x++)
                {
                    for (int k=0; k < (int)kchannels.size(); k++)
                    {
                        int channel = kchannels[k];
                        int current_pixel_y = y + spec.y;
                        int current_pixel_x = x + spec.x;

                        if (current_pixel_y >= 0 &&
                            current_pixel_x >= 0 &&
                            current_pixel_y < imgheight &&
                            current_pixel_x < imgwidth)
                        {
                            const size_t imgIdx = (y * spec.width * components) 
                                                    + (x * components) + channel;

                            const size_t cropIdx = (current_pixel_y * imgwidth * kchannels.size())
                                                    + (current_pixel_x * kchannels.size())
                                                    + channel;

                            if(spec.format==OIIO::TypeDesc::FLOAT)
                            {
                                ((float*)croppedBuf)[cropIdx] = ((float*)imgBuf)[imgIdx];
                            }
                            else if(spec.format==OIIO::TypeDesc::HALF)
                            {
                                ((half*)croppedBuf)[cropIdx] = ((half*)imgBuf)[imgIdx];
                            }
                            else if(spec.format==OIIO::TypeDesc::UINT16)
                            {
                                ((uint16_t*)croppedBuf)[cropIdx] = ((uint16_t*)imgBuf)[imgIdx];
                            }
                            else if(spec.format==OIIO::TypeDesc::UINT8)
                            {
                                ((uint8_t*)croppedBuf)[cropIdx] = ((uint8_t*)imgBuf)[imgIdx];
                            }
                            else
                            {
                                std::cerr << "ERROR: Unsupported image type: " 
                                          << spec.format << std::endl;
                                exit(1);
                            }
                        }
                    }
                }
            }

            components = (int)(kchannels.size());

            img = std::move(croppedImg);
        }
    }
    catch(...)
    {
        std::cerr << "ERROR: Loading file failed" << std::endl;
        exit(1);
    }

    // Initialize GPU
    if (usegpu || usegpuLegacy)
    {
        GPUManagement & gpuMgmt = GPUManagement::Instance();
        gpuMgmt.init(verbose);
        gpuMgmt.prepareImage((float *)img.getBuffer(), imgwidth, imgheight, components);
    }

    // Process the image
    try
    {
        // Load the current config.
        OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();

        // Get the processor
        OCIO::ConstProcessorRcPtr processor;

        try
        {
            if (useLut)
            {
                // Create the OCIO processor for the specified transform.
                OCIO::FileTransformRcPtr t = OCIO::FileTransform::Create();
                t->setSrc(lutFile);
                t->setInterpolation(OCIO::INTERP_BEST);
    
                processor = config->getProcessor(t);
            }
            else if (useDisplayView)
            {
                OCIO::DisplayTransformRcPtr t = OCIO::DisplayTransform::Create();
                t->setInputColorSpaceName(inputcolorspace);
                t->setDisplay(display);
                t->setView(view);
                processor = config->getProcessor(t);
            }
            else
            {
                processor = config->getProcessor(inputcolorspace, outputcolorspace);
            }
        }
        catch (const OCIO::Exception & e)
        {
            std::cout << "ERROR: OCIO failed with: " << e.what() << std::endl;
            exit(1);
        }
        catch (...)
        {
            std::cout << "ERROR: Creating processor unknown failure" << std::endl;
            exit(1);
        }

        if (usegpu || usegpuLegacy)
        {
            GPUManagement & gpuMgmt = GPUManagement::Instance();
            // Get the GPU shader program from the processor and set GPU to use it
            gpuMgmt.updateGPUShader(processor, usegpuLegacy, outputgpuInfo);

            // Run the GPU shader on the image
            gpuMgmt.processImage();

            // Read the result
            gpuMgmt.readImage((float *)img.getBuffer());
        }
        else
        {
            const OCIO::BitDepth bitDepth = OCIO::GetBitDepth(spec);

            OCIO::ConstCPUProcessorRcPtr cpuProcessor 
                = processor->getOptimizedCPUProcessor(bitDepth, bitDepth,
                                                      OCIO::OPTIMIZATION_DEFAULT);

            const std::chrono::high_resolution_clock::time_point start
                = std::chrono::high_resolution_clock::now();

            OCIO::ImageDescRcPtr imgDesc = OCIO::CreateImageDesc(spec, img);
            cpuProcessor->apply(*imgDesc);

            if(verbose)
            {
                const std::chrono::high_resolution_clock::time_point end
                    = std::chrono::high_resolution_clock::now();

                std::chrono::duration<float, std::milli> duration = end - start;

                std::cout << std::endl;
                std::cout << "CPU processing took: " 
                          << duration.count()
                          <<  " ms" << std::endl;
            }
        }
    }
    catch(OCIO::Exception & exception)
    {
        std::cerr << "ERROR: OCIO failed with: " << exception.what() << std::endl;
        exit(1);
    }
    catch(...)
    {
        std::cerr << "ERROR: Unknown error processing the image" << std::endl;
        exit(1);
    }

    //
    // set the provided OpenImageIO attributes
    //
    bool parseerror = false;
    for(unsigned int i=0; i<floatAttrs.size(); ++i)
    {
        std::string name, value;
        float fval = 0.0f;

        if(!ParseNameValuePair(name, value, floatAttrs[i]) ||
           !StringToFloat(&fval,value.c_str()))
        {
            std::cerr << "ERROR: Attribute string '" << floatAttrs[i]
                      << "' should be in the form name=floatvalue" << std::endl;
            parseerror = true;
            continue;
        }

        spec.attribute(name, fval);
    }

    for(unsigned int i=0; i<intAttrs.size(); ++i)
    {
        std::string name, value;
        int ival = 0;
        if(!ParseNameValuePair(name, value, intAttrs[i]) ||
           !StringToInt(&ival,value.c_str()))
        {
            std::cerr << "ERROR: Attribute string '" << intAttrs[i]
                      << "' should be in the form name=intvalue" << std::endl;
            parseerror = true;
            continue;
        }

        spec.attribute(name, ival);
    }

    for(unsigned int i=0; i<stringAttrs.size(); ++i)
    {
        std::string name, value;
        if(!ParseNameValuePair(name, value, stringAttrs[i]))
        {
            std::cerr << "ERROR: Attribute string '" << stringAttrs[i]
                      << "' should be in the form name=value" << std::endl;
            parseerror = true;
            continue;
        }

        spec.attribute(name, value);
    }

    if(parseerror)
    {
        exit(1);
    }

    // Write out the result
    try
    {
#if OIIO_VERSION < 10903
        OIIO::ImageOutput* f = OIIO::ImageOutput::create(outputimage);
#else
        auto f = OIIO::ImageOutput::create(outputimage);
#endif
        if(!f)
        {
            std::cerr << "ERROR: Could not create output input" << std::endl;
            exit(1);
        }

        f->open(outputimage, spec);

        if(!f->write_image(spec.format, img.getBuffer()))
        {
            std::cerr << "ERROR: Writing \"" << outputimage << "\" failed with: "
                      << f->geterror() << std::endl;
            exit(1);
        }

        f->close();
#if OIIO_VERSION < 10903
        OIIO::ImageOutput::destroy(f);
#endif
    }
    catch(...)
    {
        std::cerr << "ERROR: Writing file \"" << outputimage << "\"" << std::endl;
        exit(1);
    }

    std::cout << std::endl;
    std::cout << "Wrote " << outputimage << std::endl;

    return 0;
}


// Parse name=value parts
// return true on success

bool ParseNameValuePair(std::string& name,
                        std::string& value,
                        const std::string& input)
{
    // split string into name=value 
    size_t pos = input.find('=');
    if(pos==std::string::npos) return false;

    name = input.substr(0,pos);
    value = input.substr(pos+1);
    return true;
}

// return true on success
bool StringToFloat(float * fval, const char * str)
{
    if(!str) return false;

    std::istringstream inputStringstream(str);
    float x;
    if(!(inputStringstream >> x))
    {
        return false;
    }

    if(fval) *fval = x;
    return true;
}

bool StringToInt(int * ival, const char * str)
{
    if(!str) return false;

    std::istringstream inputStringstream(str);
    int x;
    if(!(inputStringstream >> x))
    {
        return false;
    }

    if(ival) *ival = x;
    return true;
}

bool StringToVector(std::vector<int> * ivector, const char * str)
{
    std::stringstream ss(str);
    int i;
    while (ss >> i)
    {
        ivector->push_back(i);
        if (ss.peek() == ',')
        {
          ss.ignore();
        }
    }
    return ivector->size() != 0;
}




