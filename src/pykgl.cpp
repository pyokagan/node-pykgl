// TODO: handle OpenGL context lost (set invalidated flags etc.)
// TODO: Add typed arrays support
#include <node.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <GL/glew.h>
#include <GL/gl.h>
#include "GLSLANG/ShaderLang.h"

#define LOGGING

using namespace v8;
using namespace node;


// ===========================================================
// ANGLE support

namespace angle {

    static ShBuiltInResources resources;

    void init() {
        // Initialise ANGLE
        ShInitialize();

        ShInitBuiltInResources(&resources);

        resources.MaxVertexAttribs = 8;
        resources.MaxVertexUniformVectors = 128;
        resources.MaxVaryingVectors = 8;
        resources.MaxVertexTextureImageUnits = 0;
        resources.MaxCombinedTextureImageUnits = 8;
        resources.MaxTextureImageUnits = 8;
        resources.MaxFragmentUniformVectors = 16;
        resources.MaxDrawBuffers = 1;

        resources.OES_standard_derivatives = 0;
        resources.OES_EGL_image_external = 0;
    }

    void destroy() {
        ShFinalize();
    }

}
// ==========================================================
// WebGL-specific defines

const GLenum GL_UNPACK_FLIP_Y_WEBGL            = 0x9240;
const GLenum GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL = 0x9241;
const GLenum GL_CONTEXT_LOST_WEBGL             = 0x9242;
const GLenum GL_UNPACK_COLORSPACE_CONVERSION_WEBGL = 0x9243;
const GLenum GL_BROWSER_DEFAULT_WEBGL          = 0x9244;


// ==========================================================

#define JS_METHOD(name) Handle<Value> name(const Arguments &args)
#define JS_GET(name) Handle<Value> name(Local<String> property, const AccessorInfo &info)
#define JS_SET(name)  void name(Local<String> property, Local<Value> value, const AccessorInfo &info)

void ReleasePersistent(Persistent<Value> p) {
    if (!p.IsEmpty()) {
        p.Dispose();
        p.Clear();
    }
}

v8::Handle<v8::Value> ThrowError(const char* msg) {
  return v8::ThrowException(v8::Exception::Error(v8::String::New(msg)));
}

inline int sizeOfArrayElementForType(v8::ExternalArrayType type) {
    switch (type) {
        case v8::kExternalByteArray:
        case v8::kExternalUnsignedByteArray:
            return 1;
        case v8::kExternalShortArray:
        case v8::kExternalUnsignedShortArray:
            return 2;
        case v8::kExternalIntArray:
        case v8::kExternalUnsignedIntArray:
        case v8::kExternalFloatArray:
            return 4;
        default:
            return 0;
    }
}

/*
inline GLenum glElementDataTypeForArrayType(ExternalArrayType type) {
    switch(type) {
        case kExternalByteArray:
        case kExternalUnsignedByteArray:
            return GL_BYTE;
        case kExternalShortArray:
        case kExternalUnsignedShortArray:
            return GL_SHORT;
        case kExternalIntArray:
        case kExternalUnsignedIntArray:
            return GL_INT;
        case kExternalFloatArray:
            return GL_FLOAT;
        default:
            return GL_DOUBLE;
    }
}*/

template<typename Type>
Type* getArrayData(Local<Value> arg, int* num = NULL) {
    Type *data=NULL;
    if(num) *num=0;

    if(!arg->IsNull()) {
        if(arg->IsArray()) {
            Local<Array> arr = Array::Cast(*arg);
            if(num) *num=arr->Length();
                data = reinterpret_cast<Type*>(arr->GetIndexedPropertiesExternalArrayData());
        }
        else if(arg->IsObject()) {
            if(num) *num = arg->ToObject()->GetIndexedPropertiesExternalArrayDataLength();
            data = reinterpret_cast<Type*>(arg->ToObject()->GetIndexedPropertiesExternalArrayData());
        }
        else
            ThrowException(String::New("Bad array argument"));
    }
    return data;
}

inline void *getImageData(Local<Value> arg) {
  void *pixels = NULL;
  if (!arg->IsNull()) {
    Local<Object> obj = Local<Object>::Cast(arg);
    if (!obj->IsObject())
      ThrowException(String::New("Bad texture argument"));

    pixels = obj->GetIndexedPropertiesExternalArrayData();
  }
  return pixels;
}

Handle<Value> newTypedArray(const char *name, unsigned int length) {
    HandleScope scope;
    Local<Object> globalObj = Context::GetCurrent()->Global();
    Local<Value> _arrayConstructor = globalObj->Get(String::NewSymbol(name));
    if (!_arrayConstructor->IsFunction()) {
        std::cerr << "Could not find " << name << "\n";
        return scope.Close(ThrowError("Could not find typed array type"));
    }
    Local<Function> arrayConstructor = Local<Function>::Cast(_arrayConstructor);
    Handle<Value> argv[1] = {Integer::New(length)};
    Local<Object> array = arrayConstructor->NewInstance(1, argv);
    return scope.Close(array);
}


// ================================================================

void _makeContextCurrent(Handle<Value> arg) {
    //Call this at the beginning of each function. It will call
    //this.makeContextCurrent to make the OpenGL context current.
    HandleScope scope;

    Local<Object> context = Local<Object>(arg->ToObject());
    Local<Value> func = context->Get(String::NewSymbol("makeContextCurrent"));
    if (func->IsFunction()) {
        const unsigned argc = 0;
        Local<Function>::Cast(func)->Call(context, argc, NULL);
    }
}

// ================================================================

class WebGLRenderingContext;

class WebGLObject : public ObjectWrap {
    public:
        static void Init(Handle<Object> target);

    private:
        WebGLObject();
        ~WebGLObject();
};

class WebGLTexture : public ObjectWrap {
    public:
        static void Init(Handle<Object> target);

        GLuint gltexture;
        static Persistent<Function> constructor;
        
    private:
        WebGLTexture(WebGLRenderingContext *ctx) : context(ctx) {
            glGenTextures(1, &gltexture);
        }
        ~WebGLTexture() {
            std::cerr << "texture Deleted\n";
            glDeleteTextures(1, &gltexture);
        }

        WebGLRenderingContext *context;

        static JS_METHOD(New); //Note: new Should not be called from javascript
};

Persistent<Function> WebGLTexture::constructor;

JS_METHOD(WebGLTexture::New) {
    // new WebGLTexture(WebGLRenderingContext context)
    HandleScope scope;

    // Get WebGLRenderingContext from args[0]
    WebGLRenderingContext *ctx = ObjectWrap::Unwrap<WebGLRenderingContext>(args[0]->ToObject());
    WebGLTexture *obj = new WebGLTexture(ctx);
    obj->Wrap(args.This());

    return args.This();
}

void WebGLTexture::Init(Handle<Object> target) {
    HandleScope scope;

    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("WebGLTexture"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    constructor = Persistent<Function>::New(tpl->GetFunction());
}

// ===================================================

class WebGLShader : public ObjectWrap {
    public:
        static void Init(Handle<Object> target);

        GLuint glshader;
        GLenum gltype;
        std::string source; //Shader Source
        std::string angle_log; //Angle Shader log

        static Persistent<Function> constructor;
    private:

        WebGLShader(Handle<Object> ctx, GLenum type): gltype(type) {
            context = Persistent<Object>::New(ctx);
            //_makeContextCurrent(context);
            if (GLEW_ARB_shader_objects)
                glshader = glCreateShaderObjectARB(type);
            else if (GLEW_VERSION_2_0) 
                glshader = glCreateShader(type);
            if (glshader == 0) {
                std::cerr << "WARNING: Error creating shader object!" << std::endl;
            }
        }

        ~WebGLShader() {
            //_makeContextCurrent(context);
            if (GLEW_ARB_shader_objects)
                glDeleteObjectARB(glshader);
            else if (GLEW_VERSION_2_0)
                glDeleteShader(glshader);
            if (!context.IsEmpty()) {
                context.Dispose();
                context.Clear();
            }
        }

        Persistent<Object> context;

        static JS_METHOD(New); //Note: new should not be called from javascript
};

Persistent<Function> WebGLShader::constructor;

JS_METHOD(WebGLShader::New) {
    // new WebGLShader(WebGLRenderingContext context, GLenum type)
    HandleScope scope;

    GLenum type = args[1]->Int32Value();

    WebGLShader *obj = new WebGLShader(args[0]->ToObject(), type);
    obj->Wrap(args.This());

    return args.This();
}

void WebGLShader::Init(Handle<Object> target) {
    HandleScope scope;

    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("WebGLShader"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    constructor = Persistent<Function>::New(tpl->GetFunction());
}

// =========================================================

class WebGLProgram : public ObjectWrap {
    public:
        static void Init(Handle<Object> target);

        GLuint glprogram;
        static Persistent<Function> constructor;

        static WebGLProgram *Ptr(Handle<Value> x);

        static GLuint Val(Handle<Value> x);

    private:
        WebGLProgram(Handle<Object> ctx) {
            context = Persistent<Object>::New(ctx);
            //_makeContextCurrent(context);
            if (GLEW_ARB_shader_objects) 
                glprogram = glCreateProgramObjectARB();
            else if (GLEW_VERSION_2_0) 
                glprogram = glCreateProgram();
        }
        ~WebGLProgram() {
            //_makeContextCurrent(context);
            if (GLEW_ARB_shader_objects)
                glDeleteObjectARB(glprogram);
            else if (GLEW_VERSION_2_0)
                glDeleteProgram(glprogram);
            if (!context.IsEmpty()) {
                context.Dispose();
                context.Clear();
            }
        }
        Persistent<Object> context;
        
        static JS_METHOD(New);
};

Persistent<Function> WebGLProgram::constructor;

WebGLProgram *WebGLProgram::Ptr(Handle<Value> x) {
    return ObjectWrap::Unwrap<WebGLProgram>(x->ToObject());
}

GLuint WebGLProgram::Val(Handle<Value> x) {
    return Ptr(x)->glprogram;
}


JS_METHOD(WebGLProgram::New) {
    // new WebGLProgram(WebGLRenderingContext context);
    HandleScope scope;

    WebGLProgram *obj = new WebGLProgram(args[0]->ToObject());
    obj->Wrap(args.This());

    return args.This();
}

void WebGLProgram::Init(Handle<Object> target) {
    HandleScope scope;

    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("WebGLProgram"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    constructor = Persistent<Function>::New(tpl->GetFunction());
}

// =========================================================

void bb4glGenBuffers(GLsizei n, GLuint *buffers) {
    if (GLEW_ARB_vertex_buffer_object) glGenBuffersARB(n, buffers);
    else if (GLEW_VERSION_2_0) glGenBuffers(n, buffers);
}

void bb4glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    if (GLEW_ARB_vertex_buffer_object) glDeleteBuffersARB(n, buffers);
    else if (GLEW_VERSION_2_0) glDeleteBuffers(n, buffers);
}

void bb4glBindBuffer(GLenum target, GLuint buffer) {
    if (GLEW_ARB_vertex_buffer_object) glBindBufferARB(target, buffer);
    else if (GLEW_VERSION_2_0) glBindBuffer(target, buffer);
}

void bb4glBufferData(GLenum target, GLsizeiptr size, const void *data,
        GLenum usage) {
    if (GLEW_ARB_vertex_buffer_object) glBufferDataARB(target, size, data, usage);
    else if (GLEW_VERSION_2_0) glBufferData(target, size, data, usage);
}


class WebGLBuffer : public ObjectWrap {
    public:
        static void Init(Handle<Object> target);

        GLuint glbuffer;
        Persistent<Object> context;
        static Persistent<Function> constructor;
    private:
        WebGLBuffer(Handle<Object> ctx) {
            HandleScope scope;
            context = Persistent<Object>::New(ctx);
            //_makeContextCurrent(context);
            bb4glGenBuffers(1, &glbuffer);
        }
        ~WebGLBuffer() {
            HandleScope scope;
            //_makeContextCurrent(context);
            bb4glDeleteBuffers(1, &glbuffer);
            if (!context.IsEmpty()) {
                context.Dispose();
                context.Clear();
            }
        }
        static JS_METHOD(New);
};

Persistent<Function> WebGLBuffer::constructor;


JS_METHOD(WebGLBuffer::New) {
    HandleScope scope;

    WebGLBuffer *obj = new WebGLBuffer(args[0]->ToObject());
    obj->Wrap(args.This());

    return args.This();
}

void WebGLBuffer::Init(Handle<Object> target) {
    HandleScope scope;

    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("WebGLBuffer"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    constructor = Persistent<Function>::New(tpl->GetFunction());
}

// =========================================================

class WebGLUniformLocation: public ObjectWrap {
    public:
        static void Init(Handle<Object> target);

        GLint gluniform;
        static Persistent<Function> constructor;

        static WebGLUniformLocation *Ptr(Handle<Value> x);

        static GLint Val(Handle<Value> x);

        GLenum getType();

        static void onProgramDestroy (Persistent<Value> object, void *thisptr);

    private:
        GLenum type; //Cached Uniform Type

        WebGLUniformLocation(Handle<Object> ctx, Handle<Object> _program, GLuint _gluniform) : gluniform(_gluniform), type(0) {
            context = Persistent<Object>::New(ctx);
            program = Persistent<Object>::New(_program); //associated WebGLProgram
            // Weak reference to WebGLProgram only to allow it to be destroyed
            program.MakeWeak(reinterpret_cast<void*>(this), WebGLUniformLocation::onProgramDestroy); 
        }

        ~WebGLUniformLocation() {
            if (!context.IsEmpty()) {
                context.Dispose();
                context.Clear();
            }
        }


        Persistent<Object> context;
        Persistent<Object> program;

        static JS_METHOD(New);
};

Persistent<Function> WebGLUniformLocation::constructor;

WebGLUniformLocation *WebGLUniformLocation::Ptr(Handle<Value> x) {
    return ObjectWrap::Unwrap<WebGLUniformLocation>(x->ToObject());
}

GLint WebGLUniformLocation::Val(Handle<Value> x) {
    return Ptr(x)->gluniform;
}

void WebGLUniformLocation::onProgramDestroy(Persistent<Value> object, void *thisptr) {
    object.Dispose();
    object.Clear();
    // TODO: Set a self.noprogramliao flag or something
    //WebGLUniformLocation *self = reinterpret_cast<WebGLUniformLocation*>(thisptr);
}

GLenum WebGLUniformLocation::getType() {
    if (type != 0) return type;
    
    char outName[1024];
    GLsizei outLength;
    GLsizei outSize;
    GLuint _program = WebGLProgram::Val(program);

    glGetActiveUniform(_program, gluniform, 1024, &outLength, &outSize, &type, outName);
    if (outLength == 0) return 0;
    return type;
}

JS_METHOD(WebGLUniformLocation::New) {
    // new WebGLUniformLocation(WebGLRenderingContext context)
    HandleScope scope;

    WebGLUniformLocation *obj = new WebGLUniformLocation(args[0]->ToObject(), args[1]->ToObject(), args[2]->Int32Value());
    obj->Wrap(args.This());

    return args.This();
}

void WebGLUniformLocation::Init(Handle<Object> target) {
    HandleScope scope;

    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("WebGLUniformLocation"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    constructor = Persistent<Function>::New(tpl->GetFunction());
}


// =========================================================

class WebGLRenderingContext : public ObjectWrap {
    // TODO: Support multiple WebGL contexts
    public:
        static void Init(Handle<Object> target);

        WebGLRenderingContext() {}
        ~WebGLRenderingContext() {// TODO: Maybe ensure all textures have been destroyed?
        }
    private:

        static Handle<Value> New(const Arguments &args);
        static JS_METHOD(createTexture);
        static JS_METHOD(bindTexture);
        static JS_METHOD(texParameteri);
        static JS_METHOD(texImage2D);
        static JS_METHOD(makeContextCurrent);
        static JS_METHOD(destroyContext);
        static JS_METHOD(createShader);
        static JS_METHOD(shaderSource);
        static JS_METHOD(compileShader);
        static JS_METHOD(getShaderParameter);
        static JS_METHOD(createProgram);
        static JS_METHOD(attachShader);
        static JS_METHOD(linkProgram);
        static JS_METHOD(getProgramParameter);
        static JS_METHOD(useProgram);
        static JS_METHOD(getAttribLocation);
        static JS_METHOD(enableVertexAttribArray);
        static JS_METHOD(getUniformLocation);
        static JS_METHOD(createBuffer);
        static JS_METHOD(bindBuffer);
        static JS_METHOD(bufferData);
        static JS_METHOD(clearColor);
        static JS_METHOD(enable);
        static JS_METHOD(disable);
        static JS_METHOD(viewport);
        static JS_METHOD(clear);
        static JS_METHOD(vertexAttribPointer);
        static JS_METHOD(uniformMatrix4fv);
        static JS_METHOD(drawArrays);
        static JS_METHOD(clearDepth);
        static JS_METHOD(depthFunc);
        static JS_METHOD(getShaderInfoLog);
        static JS_METHOD(getProgramInfoLog);
        static JS_METHOD(drawElements);
        static JS_METHOD(activeTexture);
        static JS_METHOD(uniform1i);
        static JS_METHOD(pixelStorei);
        static JS_METHOD(getActiveUniform);
        static JS_METHOD(getUniform);
        static JS_METHOD(getActiveAttrib);
        static JS_METHOD(depthMask);
        static JS_METHOD(disableVertexAttribArray);
        static JS_METHOD(getParameter);
        static JS_METHOD(generateMipmap);

        // FIXED FUNCTION
        static JS_METHOD(matrixMode);
        static JS_METHOD(loadMatrix);
        static JS_METHOD(loadIdentity);
        static JS_METHOD(enableClientState);
        static JS_METHOD(disableClientState);
        static JS_METHOD(vertexPointer);
        static JS_METHOD(normalPointer);
        static JS_METHOD(texCoordPointer);
        static JS_METHOD(clientActiveTexture);
        static JS_METHOD(texGen);
        

        Persistent<Object> canvas; //Reference to the canvas element which created this context
        static JS_GET(get_canvas);
        static JS_SET(set_canvas);
        // TODO: drawingBufferWidth, drawingBufferHeight. This should be equal to the window width/height actually.
};

// ----------------------
Handle<Value> WebGLRenderingContext::New(const Arguments &args) {
    // new WebGLRenderingContext(CanvasElement canvas, WebGLContextAttributes attrs)
    HandleScope scope;
    WebGLRenderingContext *ctx;

    ctx = new WebGLRenderingContext();
    if (args[0]->IsObject()) {
        ctx->canvas = Persistent<Object>::New(args[0]->ToObject());
    }
    ctx->Wrap(args.This());
    
    // Init GLEW
    GLenum err = glewInit();
    if (GLEW_OK != err) {
        // glewInit failed. Something is seriously wrong.
        std::cerr << glewGetErrorString(err) << std::endl;
    }

    return scope.Close(args.This());
}

JS_METHOD(WebGLRenderingContext::makeContextCurrent) {
    // makeContextCurrent()
    HandleScope scope;

    // Do nothing
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::destroyContext) {
    // destroyContext()
    HandleScope scope;

    // Do nothing
    return scope.Close(Undefined());
}

JS_GET(WebGLRenderingContext::get_canvas) {
    // Return canvas
    WebGLRenderingContext *ctx = ObjectWrap::Unwrap<WebGLRenderingContext>(info.This());
    return ctx->canvas;
}

JS_SET(WebGLRenderingContext::set_canvas) {
    HandleScope scope;
    // Not allowed. Don't do anything
}


// ==========================================================

JS_METHOD(WebGLRenderingContext::createTexture) {
    HandleScope scope;

    const int argc = 1;
    Handle<Value> argv[argc] = {args.This()};
    Local<Object> instance = WebGLTexture::constructor->NewInstance(argc, argv);
    return scope.Close(instance);
}

JS_METHOD(WebGLRenderingContext::texParameteri) {
    HandleScope scope;

    int target = args[0]->Int32Value();
    int pname = args[1]->Int32Value();
    int param = args[2]->Int32Value();

    glTexParameteri(target, pname, param);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::bindTexture) {
    // void bindTexture(GLenum target, WebGLTexture? texture);
    // NOTE: In this case, texture == NULL is the same as
    // gltexture = 0.
    // OpenGL: void glBindTexture(GLenum target,  GLuint texture);
    HandleScope scope;

    GLenum target = args[0]->Int32Value();
    GLuint texture = args[1]->IsObject() ? ObjectWrap::Unwrap<WebGLTexture>(args[1]->ToObject())->gltexture : 0;

    glBindTexture(target, texture);
    return scope.Close(Undefined());
}


JS_METHOD(WebGLRenderingContext::texImage2D) {
    // texImage2D(GLenum target, GLint level, GLenum internalFormat,
    // GLsizei width, GLsizei height, GLint border, GLenum format,
    // GLenum type, ArrayBufferView? pixels);
    // texImage2D(GLenum target, GLint level, GLenum internalformat, 
    // GLenum format, GLenum type, ImageData? pixels);
    // texImage2D(GLenum target, GLint level, GLenum internalformat,
    // GLenum format, GLenum type, HTMLImageElement image);
    // OpenGL: void glTexImage2D(GLenum target,  GLint level,  GLint internalformat,  GLsizei width,  GLsizei height,  GLint border,  GLenum format,  GLenum type,  const GLvoid * data);
    HandleScope scope;

    GLenum target = args[0]->Int32Value();
    GLint level = args[1]->Int32Value();
    GLint internalformat = args[2]->Int32Value();
    if (args[5]->IsObject()) {
        return ThrowError("texImage2D: DOM elements not supported.");
    } else {
        GLsizei width = args[3]->Int32Value();
        GLsizei height = args[4]->Int32Value();
        GLint border = args[5]->Int32Value();
        GLenum format = args[6]->Int32Value();
        GLenum type = args[7]->Int32Value();
        void *pixels = getImageData(args[8]);

        glTexImage2D(target, level, internalformat, width, height, border,
                format, type, pixels);
    }
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::createShader) {
    // WebGLShader? createShader(type) 
    HandleScope scope;

    const unsigned argc = 2;
    Handle<Value> argv[argc] = {args.This(), args[0]};
    Local<Object> instance = WebGLShader::constructor->NewInstance(argc, argv);
    return scope.Close(instance);
}

JS_METHOD(WebGLRenderingContext::shaderSource) {
    HandleScope scope;

    WebGLShader *obj = ObjectWrap::Unwrap<WebGLShader>(args[0]->ToObject());
    String::Utf8Value code(args[1]);
    // We do not call glShaderSource, but just save it for now so we
    // can translate it at compileShader.
    obj->source = *code;
    /*
    int id = obj->glshader;
    String::Utf8Value code(args[1]);
    const char *codes[1] = {*code};
    GLint length = code.length();
    glShaderSource(id, 1, codes, &length);
    */
    return scope.Close(Undefined());
}

void bb4glShaderSource(GLuint shader, GLsizei count, const GLchar **string, const GLint *length) {
    if (GLEW_ARB_shader_objects)
        glShaderSourceARB(shader, count, string, length);
    else if (GLEW_VERSION_2_0)
        glShaderSource(shader, count, string, length);
}

void bb4glCompileShader(GLuint shader) {
    if (GLEW_ARB_shader_objects)
        glCompileShaderARB(shader);
    else if (GLEW_VERSION_2_0)
        glCompileShader(shader);
}

JS_METHOD(WebGLRenderingContext::compileShader) {
    // void compileShader(WebGLShader? shader);
    HandleScope scope;

    WebGLShader *obj = ObjectWrap::Unwrap<WebGLShader>(args[0]->ToObject());
    // Construct compiler
    ShHandle angle_compiler;
    ShShaderType angle_shader_type;
    switch(obj->gltype) {
        case GL_FRAGMENT_SHADER:
            angle_shader_type = SH_FRAGMENT_SHADER;
            break;
        case GL_VERTEX_SHADER:
            angle_shader_type = SH_VERTEX_SHADER;
            break;
        default:
            return ThrowError("compileShader: Unknown Shader type!");
    }
    angle_compiler = ShConstructCompiler(angle_shader_type,
            SH_GLES2_SPEC, SH_GLSL_OUTPUT, &angle::resources);
    if (!angle_compiler) {
        return ThrowError("compileShader: Could not ShConstructCompiler");
    }
    int compile_options = SH_OBJECT_CODE;
    const char *sources[1] = {obj->source.c_str()};
    int result = ShCompile(angle_compiler, sources, 1, compile_options);

    // Get Compilation Log
    size_t angle_log_len;
    ShGetInfo(angle_compiler, SH_INFO_LOG_LENGTH, &angle_log_len);
    // Allocate buffer
    char *angle_log_buffer = new char[angle_log_len];
    ShGetInfoLog(angle_compiler, angle_log_buffer);
    obj->angle_log = angle_log_buffer; //Copy to string
    delete[] angle_log_buffer; //Delete buffer


    if (!result) {
        // Translation failed, so don't continue
        ShDestruct(angle_compiler);
        return scope.Close(Undefined());
    }

    // Now get the translated code
    size_t angle_code_len;
    ShGetInfo(angle_compiler, SH_OBJECT_CODE_LENGTH, &angle_code_len);
    char *angle_code_buffer = new char[angle_code_len];
    ShGetObjectCode(angle_compiler, angle_code_buffer);

    const char *codes[1] = {angle_code_buffer};
    GLint angle_code_len_int = static_cast<GLint>(angle_code_len);
    bb4glShaderSource(obj->glshader, 1, codes, &angle_code_len_int);
    bb4glCompileShader(obj->glshader);
    delete[] angle_code_buffer;

    ShDestruct(angle_compiler);

    return scope.Close(Undefined());
}

void bb4glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    if (GLEW_ARB_shader_objects)
        glGetObjectParameterivARB(shader, pname, params);
    else if (GLEW_VERSION_2_0)
        glGetShaderiv(shader, pname, params);
}

JS_METHOD(WebGLRenderingContext::getShaderParameter) {
    HandleScope scope;

    WebGLShader *obj = ObjectWrap::Unwrap<WebGLShader>(args[0]->ToObject());
    int shader = obj->glshader;
    int pname = args[1]->Int32Value();
    int value = 0;

    switch (pname) {
        case GL_DELETE_STATUS:
        case GL_COMPILE_STATUS:
            bb4glGetShaderiv(shader, pname, &value);
            return scope.Close(Boolean::New(static_cast<bool>(value!=0)));

        case GL_SHADER_TYPE:
            bb4glGetShaderiv(shader, pname, &value);
            return scope.Close(Integer::New(static_cast<unsigned long>(value)));

        case GL_INFO_LOG_LENGTH:
        case GL_SHADER_SOURCE_LENGTH:
            bb4glGetShaderiv(shader, pname, &value);
            return scope.Close(Integer::New(static_cast<long>(value)));

        default:
            return ThrowException(Exception::TypeError(String::New("GetShaderParameter: Invalid Enum")));
    }
}

JS_METHOD(WebGLRenderingContext::createProgram) {
    HandleScope scope;

    const unsigned argc = 1;
    Handle<Value> argv[argc] = {args.This()};
    Local<Object> instance = WebGLProgram::constructor->NewInstance(argc, argv);
    return scope.Close(instance);
}

void bb4glAttachShader(GLuint program, GLuint shader) {
    if (GLEW_ARB_shader_objects)
        glAttachObjectARB(program, shader);
    else if (GLEW_VERSION_2_0)
        glAttachShader(program, shader);
}

JS_METHOD(WebGLRenderingContext::attachShader) {
    HandleScope scope;

    WebGLProgram *programobj = ObjectWrap::Unwrap<WebGLProgram>(args[0]->ToObject());
    WebGLShader *shaderobj = ObjectWrap::Unwrap<WebGLShader>(args[1]->ToObject());
    int program = programobj->glprogram;
    int shader = shaderobj->glshader;

    bb4glAttachShader(program, shader);
    return scope.Close(Undefined());
}

void bb4glLinkProgram(GLuint program) {
    if (GLEW_ARB_shader_objects)
        glLinkProgramARB(program);
    else if (GLEW_VERSION_2_0)
        glLinkProgram(program);
}

JS_METHOD(WebGLRenderingContext::linkProgram) {
    HandleScope scope;

    WebGLProgram *programobj = ObjectWrap::Unwrap<WebGLProgram>(args[0]->ToObject());
    int program = programobj->glprogram;

    bb4glLinkProgram(program);
    return scope.Close(Undefined());
}

void bb4glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    if (GLEW_ARB_shader_objects)
        glGetObjectParameterivARB(program, pname, params);
    else if (GL_VERSION_2_0) 
        glGetProgramiv(program, pname, params);
}

JS_METHOD(WebGLRenderingContext::getProgramParameter) {
    HandleScope scope;

    WebGLProgram *programobj = ObjectWrap::Unwrap<WebGLProgram>(args[0]->ToObject());
    int program = programobj->glprogram;
    int pname = args[1]->Int32Value();

    int value = 0;
    switch (pname) {
        case GL_DELETE_STATUS:
        case GL_LINK_STATUS:
        case GL_VALIDATE_STATUS:
            bb4glGetProgramiv(program, pname, &value);
            return scope.Close(Boolean::New(static_cast<bool>(value!=0)));
        case GL_ATTACHED_SHADERS:
        case GL_ACTIVE_ATTRIBUTES:
        case GL_ACTIVE_UNIFORMS:
            bb4glGetProgramiv(program, pname, &value);
            return scope.Close(Integer::New(static_cast<long>(value)));
        default:
            return ThrowException(Exception::TypeError(String::New("GetProgramParameter: Invalid Enum")));
    }
}

void bb4glUseProgram(GLuint program) {
    if (GLEW_ARB_shader_objects)
        glUseProgramObjectARB(program);
    else if (GLEW_VERSION_2_0)
        glUseProgram(program);
}

JS_METHOD(WebGLRenderingContext::useProgram) {
    // OpenGL: void glUseProgram(GLuint program);
    HandleScope scope;

    GLuint program;
    if (args[0]->IsNull()) {
        program = 0;
    } else {
        program = WebGLProgram::Val(args[0]);
    }
    bb4glUseProgram(program);
    return scope.Close(Undefined());
}

GLint bb4glGetAttribLocation(GLuint program, const GLchar *name) {
    if (GLEW_ARB_shader_objects && GLEW_ARB_vertex_shader)
        return glGetAttribLocationARB(program, name);
    else if (GLEW_VERSION_2_0)
        return glGetAttribLocation(program, name);
    return -1;
}


JS_METHOD(WebGLRenderingContext::getAttribLocation) {
    HandleScope scope;

    WebGLProgram *programobj = ObjectWrap::Unwrap<WebGLProgram>(args[0]->ToObject());
    int program = programobj->glprogram;
    return scope.Close(Number::New(bb4glGetAttribLocation(program, *String::Utf8Value(args[1]))));
}

void bb4glEnableVertexAttribArray(GLuint index) {
    if (GLEW_ARB_shader_objects && GLEW_ARB_vertex_shader)
        glEnableVertexAttribArrayARB(index);
    else if (GLEW_VERSION_2_0)
        glEnableVertexAttribArray(index);
}

JS_METHOD(WebGLRenderingContext::enableVertexAttribArray) {
    HandleScope scope;

    GLuint index = args[0]->Uint32Value();
    bb4glEnableVertexAttribArray(index);
    return scope.Close(Undefined());
}

GLint bb4glGetUniformLocation(GLuint program, const GLchar *name) {
    if (GLEW_ARB_shader_objects)
        return glGetUniformLocationARB(program, name);
    else if (GLEW_VERSION_2_0)
        return glGetUniformLocation(program, name);
    return -1;
}

JS_METHOD(WebGLRenderingContext::getUniformLocation) {
    HandleScope scope;

    //_makeContextCurrent(args.This());
    WebGLProgram *programobj = ObjectWrap::Unwrap<WebGLProgram>(args[0]->ToObject());
    String::AsciiValue name(args[1]);
    GLint gluniform = bb4glGetUniformLocation(programobj->glprogram, *name);
    if (gluniform != -1) {
        const int argc = 3;
        Handle<Value> argv[argc] = {args.This(), args[0]->ToObject(), Integer::New(gluniform)};
        Local<Object> instance = WebGLUniformLocation::constructor->NewInstance(argc, argv);
        return scope.Close(instance);
    } else return scope.Close(Null());
}

JS_METHOD(WebGLRenderingContext::createBuffer) {
    HandleScope scope;

    const int argc = 1;
    Handle<Value> argv[argc] = {args.This()};
    Local<Object> instance = WebGLBuffer::constructor->NewInstance(argc, argv);
    return scope.Close(instance);
}

JS_METHOD(WebGLRenderingContext::bindBuffer) {
    // void bindBuffer(GLenum target, WebGLBuffer? buffer) 
    HandleScope scope;
    //_makeContextCurrent(args.This());
    int target = args[0]->Int32Value();
    GLuint buffer;
    if (args[1]->IsNull()) {
        buffer = 0;
    } else {
        WebGLBuffer *bufferobj = ObjectWrap::Unwrap<WebGLBuffer>(args[1]->ToObject());
        buffer = bufferobj->glbuffer;
    }
    bb4glBindBuffer(target, buffer);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::bufferData) {
    // void bufferData(GLenum target, GLsizeiptr size, GLenum usage)
    // void bufferData(GLenum target, ArrayBufferView data, GLenum usage)
    // void bufferData(GLenum target, ArrayBuffer? data, GLenum usage)
    HandleScope scope;

    GLenum target = args[0]->Int32Value();
    GLenum usage = args[2]->Int32Value();
    
    if (args[1]->IsObject()) {
        Local<Object> obj = args[1]->ToObject();
        
        int element_size = sizeOfArrayElementForType(obj->GetIndexedPropertiesExternalArrayDataType());
        GLsizeiptr size = obj->GetIndexedPropertiesExternalArrayDataLength() * element_size;
        void *data = obj->GetIndexedPropertiesExternalArrayData();
        bb4glBufferData(target, size, data, usage);

    } else if (args[1]->IsNumber()) {
        GLsizeiptr size = args[1]->Uint32Value();
        bb4glBufferData(target, size, NULL, usage);
    }
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::clearColor) {
    // void clearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
    HandleScope scope;

    GLclampf red = static_cast<GLclampf>(args[0]->NumberValue());
    GLclampf green = static_cast<GLclampf>(args[1]->NumberValue());
    GLclampf blue = static_cast<GLclampf>(args[2]->NumberValue());
    GLclampf alpha = static_cast<GLclampf>(args[3]->NumberValue());

    glClearColor(red, green, blue, alpha);

    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::enable) {
    //void glEnable(GLenum cap);
    HandleScope scope;

    glEnable(args[0]->Int32Value());
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::disable) {
    // void glDisable(GLenum cap);
    HandleScope scope;
    GLenum cap = args[0]->Int32Value();
    glDisable(cap);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::viewport) {
    // void viewport(GLint x, GLint y, GLsizei width, GLsizei height);
    HandleScope scope;

    GLint x = args[0]->Int32Value();
    GLint y = args[1]->Int32Value();
    GLsizei width = args[2]->Int32Value();
    GLsizei height = args[3]->Int32Value();

    glViewport(x, y, width, height);

    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::clear) {
    // void clear(GLbitfield mask) 

    HandleScope scope;
    //_makeContextCurrent(args.This());
    glClear(args[0]->Int32Value());
    return scope.Close(Undefined());
}

void bb4glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *pointer) {
    if (GLEW_ARB_vertex_shader)
        glVertexAttribPointerARB(index, size, type, normalized, stride, pointer);
    else if (GLEW_VERSION_2_0)
        glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

JS_METHOD(WebGLRenderingContext::vertexAttribPointer) {
    //void vertexAttribPointer(GLuint indx, GLint size, GLenum type, 
    //      GLboolean normalized, GLsizei stride, GLintptr offset);
    HandleScope scope;

    GLuint indx = args[0]->Int32Value();
    GLint size = args[1]->Int32Value();
    GLenum type = args[2]->Int32Value();
    GLboolean normalized = args[3]->BooleanValue();
    GLsizei stride = args[4]->Int32Value();
    GLintptr offset = args[5]->Int32Value();

    bb4glVertexAttribPointer(indx, size, type, normalized, stride, reinterpret_cast<GLvoid*>(offset));
    return scope.Close(Undefined());
}

void bb4glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (GLEW_ARB_shader_objects) 
        glUniformMatrix4fvARB(location, count, transpose, value);
    else if (GLEW_VERSION_2_0) 
        glUniformMatrix4fv(location, count, transpose, value);
}

JS_METHOD(WebGLRenderingContext::uniformMatrix4fv) {
    //    void uniformMatrix4fv(WebGLUniformLocation? location, GLboolean transpose, 
    //                          Float32Array value);
    //    void uniformMatrix4fv(WebGLUniformLocation? location, GLboolean transpose, 
    //                          sequence<GLfloat> value);
    
    HandleScope scope;

    WebGLUniformLocation *locationobj = ObjectWrap::Unwrap<WebGLUniformLocation>(args[0]->ToObject());
    GLint location = locationobj->gluniform;
    GLboolean transpose = args[1]->BooleanValue();
    GLsizei count = 0;
    GLfloat *data = getArrayData<GLfloat>(args[2], &count);

    if (count < 16) {
        return ThrowError("Not enough data for UniformMatrix4fv");
    }

    bb4glUniformMatrix4fv(location, count / 16, transpose, data);

    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::drawArrays) {
    // void drawArrays(GLenum mode, GLint first, GLsizei count) 
    HandleScope scope;

    GLenum mode = args[0]->Int32Value();
    GLint first = args[1]->Int32Value();
    GLsizei count = args[2]->Int32Value();

    glDrawArrays(mode, first, count);

    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::clearDepth) {
    // void clearDepth(GLclampf depth);
    HandleScope scope;

    GLclampf depth = args[0]->NumberValue();
    glClearDepthf(depth);

    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::depthFunc) {
    // void depthFunc(GLenum func)
    HandleScope scope;

    GLenum func = args[0]->Int32Value();
    glDepthFunc(func);
    return scope.Close(Undefined());
}

void bb4glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
    if (GLEW_ARB_shader_objects)
        glGetInfoLogARB(shader, maxLength, length, infoLog);
    else if (GLEW_VERSION_2_0)
        glGetShaderInfoLog(shader, maxLength, length, infoLog);
}

JS_METHOD(WebGLRenderingContext::getShaderInfoLog) {
    // DOMString? getShaderInfoLog(WebGLShader? shader)
    // Returns null if any OpenGL errors are generated during the execution of this function.
    // OpenGL: void glGetShaderInfoLog(GLuint shader,  GLsizei maxLength,  GLsizei *length,  GLchar *infoLog);

    HandleScope scope;

    WebGLShader *shaderobj = ObjectWrap::Unwrap<WebGLShader>(args[0]->ToObject());
    GLuint shader = shaderobj->glshader;
    GLsizei maxLength = 1024;
    GLsizei length = 0;
    char infoLog[1024];
    bb4glGetShaderInfoLog(shader, maxLength, &length, infoLog);
    // Prepend the shader log from ANGLE
    std::string outLog(shaderobj->angle_log);
    outLog.append(infoLog);
    // TODO: Return null on error.
    return scope.Close(String::New(outLog.c_str()));
}

void bb4glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
    if (GLEW_ARB_shader_objects)
        glGetInfoLogARB(program, maxLength, length, infoLog);
    else if (GLEW_VERSION_2_0)
        glGetProgramInfoLog(program, maxLength, length, infoLog);
}

JS_METHOD(WebGLRenderingContext::getProgramInfoLog) {
    // DOMString? getProgramInfoLog(WebGLProgram? program);
    //Returns null if any OpenGL errors are generated during the execution of this function.
    // OpenGL: void glGetProgramInfoLog(GLuint program,  GLsizei maxLength,  GLsizei *length,  GLchar *infoLog);

    HandleScope scope;

    WebGLProgram *programobj = ObjectWrap::Unwrap<WebGLProgram>(args[0]->ToObject());
    GLuint program = programobj->glprogram;
    GLsizei maxLength = 1024;
    GLsizei length = 0;
    char infoLog[1024];
    bb4glGetProgramInfoLog(program, maxLength, &length, infoLog);
    return scope.Close(String::New(infoLog));
}

JS_METHOD(WebGLRenderingContext::drawElements) {
    // void drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset) 
    // If count is greater than zero, then a non-null WebGLBuffer must be bound to the ELEMENT_ARRAY_BUFFER binding point or an INVALID_OPERATION error will be generated.
    // WebGL performs additional error checking beyond that specified in OpenGL ES 2.0 during calls to drawArrays and drawElements. See Enabled Vertex Attributes and Range Checking. 
    // OpenGL: void glDrawElements(GLenum mode,  GLsizei count,  GLenum type,  const GLvoid * indices);
    HandleScope scope;

    GLenum mode = args[0]->Int32Value();
    GLsizei count = args[1]->Int32Value();
    GLenum type = args[2]->Int32Value();
    GLvoid *offset = reinterpret_cast<GLvoid*>(args[3]->Uint32Value());
    glDrawElements(mode, count, type, offset);
    return scope.Close(Undefined());
}



JS_METHOD(WebGLRenderingContext::activeTexture) {
    // void activeTexture(GLenum texture);
    HandleScope scope;
    
    glActiveTexture(args[0]->Int32Value());
    return scope.Close(Undefined());
}

void bb4glUniform1i(GLint location, GLint v0) {
    if (GLEW_ARB_shader_objects)
        glUniform1iARB(location, v0);
    else if (GLEW_VERSION_2_0)
        glUniform1i(location, v0);
}

JS_METHOD(WebGLRenderingContext::uniform1i) {
    // void uniform1i(WebGLUniformLocation? location, GLint x);
    // OpenGL: void glUniform1i(GLint location,  GLint v0);
    HandleScope scope;

    WebGLUniformLocation *locationobj = ObjectWrap::Unwrap<WebGLUniformLocation>(args[0]->ToObject());
    GLint location = locationobj->gluniform;
    GLint v0 = args[1]->Int32Value();
    bb4glUniform1i(location, v0);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::pixelStorei) {
    // void pixelStorei(GLenum pname, GLint param)
    // OpenGL:  void glPixelStorei(GLenum pname,  GLint param);
    // In addition to the parameters in the OpenGL ES 2.0 specification, 
    // the WebGL specification accepts the parameters UNPACK_FLIP_Y_WEBGL, 
    // UNPACK_PREMULTIPLY_ALPHA_WEBGL and UNPACK_COLORSPACE_CONVERSION_WEBGL. 
    
    HandleScope scope;

    GLenum pname = args[0]->Int32Value();
    GLint param = args[1]->Int32Value();

    switch(pname) {
        case GL_UNPACK_FLIP_Y_WEBGL:
            break;
        case GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL:
            break;
        case GL_UNPACK_COLORSPACE_CONVERSION_WEBGL:
            break;
        default:
            glPixelStorei(pname, param);
    }
    return scope.Close(Undefined());
}

void bb4glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    if (GLEW_ARB_shader_objects)
        glGetActiveUniformARB(program, index, bufSize, length, size, type, name);
    else if (GLEW_VERSION_2_0)
        glGetActiveUniform(program, index, bufSize, length, size, type, name);
}

JS_METHOD(WebGLRenderingContext::getActiveUniform) {
    // WebGLActiveInfo? getActiveUniform(WebGLProgram? program, GLuint index)
    // Returns a new WebGLActiveInfo object describing the size, type and name of the uniform at the passed index of the passed program object. If the passed index is out of range, generates an INVALID_VALUE error and returns null.
    // Returns null if any OpenGL errors are generated during the execution of this function.
    // OpenGL: void glGetActiveUniform(GLuint program,  GLuint index,  GLsizei bufSize,  GLsizei *length,  GLint *size,  GLenum *type,  GLchar *name);
    
    HandleScope scope;

    GLuint program = WebGLProgram::Val(args[0]);
    GLuint index = args[1]->Int32Value();

    char outName[1024];
    GLsizei outLength;
    GLenum outType;
    GLsizei outSize;

    bb4glGetActiveUniform(program, index, 1024, &outLength, &outSize, &outType, outName);
    if (outLength == 0) {
        // error occured.
        // TODO: generate INVALID_VALUE error
        return scope.Close(Null());
    }

    Local<Object> out = Object::New();
    out->Set(String::NewSymbol("size"), Integer::New(outSize), ReadOnly);
    out->Set(String::NewSymbol("type"), Integer::New(outType), ReadOnly);
    out->Set(String::NewSymbol("name"), String::New(outName), ReadOnly);
    return scope.Close(out);
}

void bb4glGetUniformfv(GLuint program, GLint location, GLfloat *params) {
    if (GLEW_ARB_shader_objects)
        glGetUniformfvARB(program, location, params);
    else if (GLEW_VERSION_2_0)
        glGetUniformfv(program, location, params);
}

JS_METHOD(WebGLRenderingContext::getUniform) {
    // any getUniform(WebGLProgram? program, WebGLUniformLocation? location)
    // OpenGL: void glGetUniformfv(GLuint program,  GLint location,  GLfloat *params); 
    // OpenGL: void glGetUniformiv(GLuint program,  GLint location,  GLint *params);

    HandleScope scope;
    GLuint program = WebGLProgram::Val(args[0]);
    GLint location = WebGLUniformLocation::Val(args[1]);
    GLenum type = WebGLUniformLocation::Ptr(args[1])->getType();

    float fdata[16];
    Local<Object> array;
    void *buf;
    switch(type) {
        case GL_FLOAT: //return GLfloat
            bb4glGetUniformfv(program, location, fdata);
            return scope.Close(Number::New(fdata[0]));
        case GL_FLOAT_MAT4:
            bb4glGetUniformfv(program, location, fdata);
            array = Local<Value>::New(newTypedArray("Float32Array", 16))->ToObject();
            buf = array->GetIndexedPropertiesExternalArrayData();
            memcpy(buf, fdata, 16 * sizeof(float));
            return scope.Close(array);
        case GL_FLOAT_VEC2:
        case GL_FLOAT_VEC3:
        case GL_FLOAT_VEC4:
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT3:
        case GL_INT:
        case GL_INT_VEC2:
        case GL_INT_VEC3:
        case GL_INT_VEC4:
        case GL_BOOL:
        case GL_BOOL_VEC2:
        case GL_BOOL_VEC3:
        case GL_BOOL_VEC4:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_CUBE:
        default:
            std::cerr << "getUniform: uniform type of " << type << "not supported YET!\n";
            return scope.Close(Null());
    }
}

void bb4glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
    if (GLEW_ARB_shader_objects && GLEW_ARB_vertex_shader) 
        glGetActiveAttribARB(program, index, bufSize, length, size, type, name);
    else if (GLEW_VERSION_2_0)
        glGetActiveAttrib(program, index, bufSize, length, size, type, name);
}

JS_METHOD(WebGLRenderingContext::getActiveAttrib) {
    //     WebGLActiveInfo? getActiveAttrib(WebGLProgram? program, GLuint index);
    //OpenGL:  void glGetActiveAttrib(GLuint program,  GLuint index,  GLsizei bufSize,  GLsizei *length,  GLint *size,  GLenum *type,  GLchar *name);
    HandleScope scope;

    GLuint program = WebGLProgram::Val(args[0]);
    GLuint index = args[1]->Int32Value();
    GLsizei length;
    GLint size;
    GLenum type;
    char name[1024];

    bb4glGetActiveAttrib(program, index, 1024, &length, &size, &type, name);

    if (length == 0) {
        // error occured.
        // TODO: generate INVALID_VALUE error
        return scope.Close(Null());
    }

    Local<Object> out = Object::New();
    out->Set(String::NewSymbol("size"), Integer::New(size), ReadOnly);
    out->Set(String::NewSymbol("type"), Integer::New(type), ReadOnly);
    out->Set(String::NewSymbol("name"), String::New(name), ReadOnly);
    return scope.Close(out);
}

JS_METHOD(WebGLRenderingContext::depthMask) {
    // void depthMask(GLboolean flag);
    // OpenGL: void glDepthMask(GLboolean flag);
    HandleScope scope;

    glDepthMask(args[0]->BooleanValue());
    return scope.Close(Undefined());
}

void bb4glDisableVertexAttribArray(GLuint index) {
    if (GLEW_ARB_vertex_shader)
        glDisableVertexAttribArrayARB(index);
    else if (GLEW_VERSION_2_0)
        glDisableVertexAttribArray(index);
}

JS_METHOD(WebGLRenderingContext::disableVertexAttribArray) {
    // void disableVertexAttribArray(GLuint index)
    // OpenGL: void glDisableVertexAttribArray(GLuint index);
    HandleScope scope;

    GLuint index = args[0]->Uint32Value();
    if (index == 0) {
        std::cerr << "disableVertexAttribArray: INDEX 0!!!\n";
    }
    bb4glDisableVertexAttribArray(index);
    return scope.Close(Undefined());
}


JS_METHOD(WebGLRenderingContext::getParameter) {
    // any getParameter(GLenum pname)
    HandleScope scope;

    GLenum pname = args[0]->Int32Value();
    switch(pname) {
        case GL_VERSION:
            return scope.Close(String::New(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
        default:
            return scope.Close(Null());
    }

}

JS_METHOD(WebGLRenderingContext::generateMipmap) {
    // void generateMipmap(GLenum target);
    // OpenGL (>=3.0) void glGenerateMipmap(GLenum target);
    // OpenGL (with EXT_framebuffer_object) void GenerateMipmapEXT(enum target);
    // OpenGL (>=1.4) glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE); 
    // OpenGL (default): ????
    // See also: http://www.opengl.org/wiki/Common_Mistakes#Automatic_mipmap_generation
    HandleScope scope;

    GLenum target = args[0]->Int32Value();
    if (GLEW_EXT_framebuffer_object) {
        glGenerateMipmapEXT(target);
    } else if (GLEW_VERSION_3_0) {
        glGenerateMipmap(target);
    } else if (GLEW_VERSION_1_4) {
        glTexParameteri(target, GL_GENERATE_MIPMAP, GL_TRUE);
    } 
    // TODO: Else ????
    return scope.Close(Undefined());
}


// FIXED FUNCTION Extension Functions

JS_METHOD(WebGLRenderingContext::matrixMode) {
    // OpenGL: void glMatrixMode(GLenum  mode);
    HandleScope scope;

    GLenum mode = args[0]->Int32Value();
    glMatrixMode(mode);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::loadMatrix) {
    // void loadMatrix(Float32Array)
    // OpenGL: void glLoadMatrixf(GLfloat *m)
    HandleScope scope;

    Local<Object> obj = args[0]->ToObject();
    if (obj->GetIndexedPropertiesExternalArrayDataType() != v8::kExternalFloatArray) {
        return ThrowError("loadMatrix: Did not get Float32Array!");
    }
    if (obj->GetIndexedPropertiesExternalArrayDataLength() != 16) {
        return ThrowError("loadMatrix: array length is not 16");
    }
    GLfloat *m = static_cast<GLfloat*>(obj->GetIndexedPropertiesExternalArrayData());
    glLoadMatrixf(m);

    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::loadIdentity) {
    //OpenGL: glLoadIdentity(void);
    HandleScope scope;

    glLoadIdentity();
    return scope.Close(Undefined());

}


JS_METHOD(WebGLRenderingContext::enableClientState) {
    // void glEnableClientState(GLenum  cap);
    HandleScope scope;

    GLenum cap = args[0]->Int32Value();
    glEnableClientState(cap);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::disableClientState) {
    // void glDisableClientState(GLenum  cap);
    HandleScope scope;

    GLenum cap = args[0]->Int32Value();
    glDisableClientState(cap);
    return scope.Close(Undefined());
}


JS_METHOD(WebGLRenderingContext::vertexPointer) {
    // vertexPointer(int size, int type, int stride, int offset)
    // void glVertexPointer(GLint  size,  GLenum  type,  GLsizei  stride,  const GLvoid *  pointer);
    HandleScope scope;

    GLint size = args[0]->Int32Value();
    GLenum type = args[1]->Int32Value();
    GLsizei stride = args[2]->Int32Value();
    GLvoid *pointer = reinterpret_cast<GLvoid*>(args[3]->Int32Value());
    glVertexPointer(size, type, stride, pointer);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::normalPointer) {
    // void normalPointer(int type, int stride, int offset)
    //void glNormalPointer(GLenum  type,  GLsizei  stride,  const GLvoid *  pointer);
    HandleScope scope;

    GLenum type = args[0]->Int32Value();
    GLsizei stride = args[1]->Int32Value();
    GLvoid *pointer = reinterpret_cast<GLvoid*>(args[2]->Int32Value());
    glNormalPointer(type, stride, pointer);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::texCoordPointer) {
    // void texCoordPointer(int size, int type, int stride, int offset);
    // void glTexCoordPointer(GLint  size,  GLenum  type,  GLsizei  stride,  const GLvoid *  pointer);
    HandleScope scope;

    GLint size = args[0]->Int32Value();
    GLenum type = args[1]->Int32Value();
    GLsizei stride = args[2]->Int32Value();
    GLvoid *pointer = reinterpret_cast<GLvoid*>(args[3]->Int32Value());
    glTexCoordPointer(size, type, stride, pointer);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::clientActiveTexture) {
    // OpenGL: void glClientActiveTexture(	GLenum  	texture);
    HandleScope scope;

    GLenum texture = args[0]->Int32Value();
    glClientActiveTexture(texture);
    return scope.Close(Undefined());
}

JS_METHOD(WebGLRenderingContext::texGen) {
    // void texGen(GLenum coord, GLenum pname, any param);
    // OpenGL: void glTexGeni( GLenum coord, GLenum pname, GLint param);
    // where coord in {gl.S, gl.T, gl.R, gl.Q} and
    // pname in {gl.TEXTURE_GEN_MODE, gl.OBJECT_PLANE, gl.EYE_PLANE}
    HandleScope scope;

    GLenum coord = args[0]->Int32Value();
    GLenum pname = args[1]->Int32Value();
    Local<Object> arrayobj;
    void *data;
    if (args[2]->IsObject()) {
        arrayobj = args[2]->ToObject();
        data = arrayobj->GetIndexedPropertiesExternalArrayData();
        switch (arrayobj->GetIndexedPropertiesExternalArrayDataType()) {
            case kExternalIntArray:
            case kExternalUnsignedIntArray:
                glTexGeniv(coord, pname, static_cast<GLint*>(data));
                return scope.Close(Undefined());
            case kExternalFloatArray:
                glTexGenfv(coord, pname, static_cast<GLfloat*>(data));
                return scope.Close(Undefined());
            default:
                return ThrowError("texGen: Wrong array type");
        }
    } else if (args[2]->IsInt32()) {
        glTexGeni(coord, pname, args[2]->Int32Value());
        return scope.Close(Undefined());
    } else {
        glTexGend(coord, pname, args[2]->NumberValue());
        return scope.Close(Undefined());
    }
}



/*
JS_METHOD(WebGLRenderingContext::interleavedArrays) {
    // void interleavedArrays(GLenum format, GLsizei stride, int offset);
    // void glInterleavedArrays(GLenum  format,  GLsizei  stride,  const GLvoid *  pointer);

    HandleScope scope;

    GLenum format = args[0]->Int32Value();
    GLsizei stride = args[1]->Int32Value();
    GLvoid *pointer = reinterpret_cast<GLvoid*>(args[2]->Int32Value());
    glInterleavedArrays(format, stride, pointer);
    return scope.Close(Undefined());
}
*/



#define METHOD(name) tpl->PrototypeTemplate()->Set(String::NewSymbol(#name), FunctionTemplate::New(name)->GetFunction())
#define ACCESSOR(name) tpl->PrototypeTemplate()->SetAccessor(String::NewSymbol(#name), get_ ## name, set_ ## name)
#define JS_GL_CONSTANT(name) tpl->PrototypeTemplate()->Set(String::NewSymbol(#name), Integer::New(GL_ ## name))

extern void InitGLConstants(Handle<ObjectTemplate> target);

void WebGLRenderingContext::Init(Handle<Object> target) {
    HandleScope scope;
    // Prepare constructor template
    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    tpl->SetClassName(String::NewSymbol("WebGLRenderingContext"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    InitGLConstants(tpl->PrototypeTemplate());

    // Subclass this class and override it
    METHOD(makeContextCurrent);
    METHOD(destroyContext);

    // Set prototype values
    ACCESSOR(canvas);
    METHOD(createTexture);
    METHOD(bindTexture);
    METHOD(texParameteri);
    METHOD(texImage2D);
    METHOD(createShader);
    METHOD(shaderSource);
    METHOD(compileShader);
    METHOD(getShaderParameter);
    METHOD(attachShader);
    METHOD(linkProgram);
    METHOD(getProgramParameter);
    METHOD(createProgram);
    METHOD(useProgram);
    METHOD(getAttribLocation);
    METHOD(enableVertexAttribArray);
    METHOD(getUniformLocation);
    METHOD(uniformMatrix4fv);
    METHOD(getShaderInfoLog);
    METHOD(getProgramInfoLog);
    METHOD(uniform1i);
    METHOD(createBuffer);
    METHOD(bindBuffer);
    METHOD(bufferData);
    METHOD(clearColor);
    METHOD(enable);
    METHOD(disable);
    METHOD(viewport);
    METHOD(clear);
    METHOD(vertexAttribPointer);
    METHOD(drawArrays);
    METHOD(clearDepth);
    METHOD(depthFunc);
    METHOD(drawElements);
    METHOD(activeTexture);
    METHOD(pixelStorei);
    METHOD(getActiveUniform);
    METHOD(getUniform);
    METHOD(getActiveAttrib);
    METHOD(depthMask);
    METHOD(disableVertexAttribArray);
    METHOD(getParameter);
    METHOD(generateMipmap);

    // FIXED FUNCTION
    METHOD(matrixMode);
    METHOD(loadMatrix);
    METHOD(loadIdentity);
    METHOD(enableClientState);
    METHOD(disableClientState);
    METHOD(vertexPointer);
    METHOD(normalPointer);
    METHOD(texCoordPointer);
    METHOD(clientActiveTexture);
    METHOD(texGen);

    // WebGL-specific constants

    JS_GL_CONSTANT(UNPACK_FLIP_Y_WEBGL);
    JS_GL_CONSTANT(UNPACK_PREMULTIPLY_ALPHA_WEBGL);
    JS_GL_CONSTANT(CONTEXT_LOST_WEBGL);
    JS_GL_CONSTANT(UNPACK_COLORSPACE_CONVERSION_WEBGL);
    JS_GL_CONSTANT(BROWSER_DEFAULT_WEBGL);

    Persistent<Function> constructor = Persistent<Function>::New(tpl->GetFunction());
    target->Set(String::NewSymbol("WebGLRenderingContext"), constructor);
}


void InitModule(Handle<Object> target) {
    angle::init();
    WebGLTexture::Init(target);
    WebGLShader::Init(target);
    WebGLProgram::Init(target);
    WebGLBuffer::Init(target);
    WebGLUniformLocation::Init(target);
    WebGLRenderingContext::Init(target);
    atexit(angle::destroy);
}

NODE_MODULE(pykgl, InitModule)
