/*
 * A minimal `Napi::` compatible surface implemented over QuickJS.
 *
 * WHY THIS EXISTS
 * ---------------
 * native-gles/src/gl_bindings.cpp is 1837 lines covering all 246 GLES3 entry points.
 * It contains 952 `Napi::` references — but they collapse to a vocabulary of about a
 * dozen types, all of them plain argument unwrapping:
 *
 *   Napi::Number   (~591)  info[i].As<Napi::Number>().Int32Value()/Uint32Value()/FloatValue()
 *   Napi::CallbackInfo     the argument vector
 *   Napi::Value / String / Boolean
 *   Napi::{Uint32,Float32,Int32,Uint8}Array   used only for .Data()/.ByteLength()/.ElementLength()
 *   Napi::Buffer<uint8_t>  .Data() + memcpy
 *
 * There is no ObjectWrap, no reference counting of JS objects, no finalizers, no
 * threading, no async work, and no libuv anywhere in that file. Every call is
 * synchronous and inline on the calling thread. That makes re-binding it a header
 * problem, not a rewrite problem: define these types over QuickJS values and the
 * GL file compiles unchanged.
 *
 * SCOPE: this is NOT an N-API implementation. It is the narrow C++ veneer that
 * gl_bindings.cpp actually uses. Loading arbitrary .node addons is a separate,
 * larger job (the 84-function ABI shim) and is deliberately not attempted here.
 *
 * LIFETIME MODEL: a shim Value borrows its JSValue from the the caller argument frame
 * and never owns a reference. Values created for return (Number::New, String::New)
 * are owned by the caller, matching QuickJS convention: the binding returns them
 * straight out and the engine takes them.
 */
#ifndef JSGLQ_NAPI_SHIM_H
#define JSGLQ_NAPI_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "quickjs.h"

namespace Napi {

class Env;
class Value;
class CallbackInfo;

/* ------------------------------------------------------------------- Env ----- */

class Env {
public:
    Env(JSContext *ctx) : ctx_(ctx) {}
    operator JSContext *() const { return ctx_; }
    JSContext *ctx() const { return ctx_; }

    inline Value Undefined() const;
    inline Value Null() const;

private:
    JSContext *ctx_;
};

/* ----------------------------------------------------------------- Value ----- */

class Value {
public:
    Value() : ctx_(nullptr), v_(JS_UNDEFINED) {}
    Value(JSContext *ctx, JSValue v) : ctx_(ctx), v_(v) {}

    JSValue raw() const { return v_; }
    JSContext *ctx() const { return ctx_; }
    /* Fully qualified: an unqualified `Env Env()` changes the meaning of the class
       name inside this scope, which g++ rejects outright. */
    ::Napi::Env Env() const { return ::Napi::Env(ctx_); }

    bool IsUndefined() const { return JS_IsUndefined(v_); }
    bool IsNull()      const { return JS_IsNull(v_); }
    bool IsNumber()    const { return JS_IsNumber(v_); }
    bool IsString()    const { return JS_IsString(v_); }
    bool IsObject()    const { return JS_IsObject(v_); }
    bool IsBoolean()   const { return JS_IsBool(v_); }

    bool IsTypedArray() const {
        if (!JS_IsObject(v_)) return false;
        size_t off = 0, len = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx_, v_, &off, &len, &bpe);
        bool ok = !JS_IsException(buf);
        JS_FreeValue(ctx_, buf);
        /* JS_GetException returns the value; it must be freed or every failed
           probe leaks an object. */
        if (!ok) JS_FreeValue(ctx_, JS_GetException(ctx_));
        return ok;
    }

    /* Node's Buffer is a Uint8Array subclass; for our purposes an ArrayBufferView
       or ArrayBuffer both qualify. The Node-Buffer-only brand check in the original
       module.cpp is deliberately relaxed here — see the note in bind_gl.cpp about
       window-handle passing. */
    bool IsBuffer() const { return IsTypedArray() || IsArrayBuffer(); }

    bool IsArrayBuffer() const {
        size_t len = 0;
        uint8_t *p = JS_GetArrayBuffer(ctx_, &len, v_);
        if (!p) { JS_FreeValue(ctx_, JS_GetException(ctx_)); return false; }
        return true;
    }

    template <typename T> T As() const { return T(ctx_, v_); }

    /* Implicit conversion so `return info.Env().Undefined();` works when the
       binding's declared return type is Napi::Value. */
    operator JSValue() const { return v_; }

protected:
    JSContext *ctx_;
    JSValue v_;
};

inline Value Env::Undefined() const { return Value(ctx_, JS_UNDEFINED); }
inline Value Env::Null() const      { return Value(ctx_, JS_NULL); }

/* ---------------------------------------------------------------- Number ----- */

class Number : public Value {
public:
    Number(JSContext *ctx, JSValue v) : Value(ctx, v) {}

    static Value New(JSContext *ctx, double d) { return Value(ctx, JS_NewFloat64(ctx, d)); }
    static Value New(const ::Napi::Env &env, double d) { return New(env.ctx(), d); }

    int32_t Int32Value() const {
        int32_t out = 0;
        JS_ToInt32(ctx_, &out, v_);
        return out;
    }
    uint32_t Uint32Value() const {
        uint32_t out = 0;
        JS_ToUint32(ctx_, &out, v_);
        return out;
    }
    int64_t Int64Value() const {
        int64_t out = 0;
        JS_ToInt64(ctx_, &out, v_);
        return out;
    }
    double DoubleValue() const {
        double out = 0;
        JS_ToFloat64(ctx_, &out, v_);
        return out;
    }
    float FloatValue() const { return (float)DoubleValue(); }
};

/* --------------------------------------------------------------- Boolean ----- */

class Boolean : public ::Napi::Value {
public:
    Boolean(JSContext *ctx, JSValue v) : ::Napi::Value(ctx, v) {}
    /* node-addon-api spells the accessor `.Value()`, which shadows the base class
       name; every mention of the base type inside this class must be qualified. */
    static ::Napi::Value New(JSContext *ctx, bool b) {
        return ::Napi::Value(ctx, JS_NewBool(ctx, b));
    }
    static ::Napi::Value New(const ::Napi::Env &env, bool b) { return New(env.ctx(), b); }
    bool Value() const { return JS_ToBool(ctx_, v_) == 1; }
};

/* ---------------------------------------------------------------- String ----- */

class String : public ::Napi::Value {
public:
    String(JSContext *ctx, JSValue v) : ::Napi::Value(ctx, v) {}

    static ::Napi::Value New(JSContext *ctx, const char *s) {
        return ::Napi::Value(ctx, JS_NewString(ctx, s ? s : ""));
    }
    static ::Napi::Value New(const ::Napi::Env &env, const char *s) { return New(env.ctx(), s); }
    static ::Napi::Value New(const ::Napi::Env &env, const std::string &s) {
        return New(env.ctx(), s.c_str());
    }
    /* Explicit-length form: GL's glGetActiveUniform/Attrib return a name buffer plus
       a written length, and the buffer is NOT guaranteed NUL-terminated at that
       length. Reading to the first NUL would be a different string. */
    static ::Napi::Value New(JSContext *ctx, const char *s, size_t len) {
        return ::Napi::Value(ctx, JS_NewStringLen(ctx, s ? s : "", s ? len : 0));
    }
    static ::Napi::Value New(const ::Napi::Env &env, const char *s, size_t len) {
        return New(env.ctx(), s, len);
    }

    std::string Utf8Value() const {
        const char *c = JS_ToCString(ctx_, v_);
        std::string out(c ? c : "");
        if (c) JS_FreeCString(ctx_, c);
        return out;
    }
    operator std::string() const { return Utf8Value(); }
};

/* ------------------------------------------------------------ typed arrays --- */

/*
 * All typed-array classes expose exactly what gl_bindings.cpp uses: Data(),
 * ByteLength(), ElementLength(). The underlying access is JS_GetTypedArrayBuffer,
 * which yields the backing store plus the view's offset and length — the offset
 * matters and dropping it is how a subarray() view silently reads the wrong bytes.
 */
class TypedArrayBase : public Value {
public:
    TypedArrayBase(JSContext *ctx, JSValue v) : Value(ctx, v) { resolve(); }

    uint8_t *DataRaw() const { return data_; }
    size_t ByteLength() const { return byte_len_; }
    size_t BytesPerElement() const { return bpe_; }
    size_t ElementLength() const { return bpe_ ? byte_len_ / bpe_ : 0; }

protected:
    void resolve() {
        data_ = nullptr; byte_len_ = 0; bpe_ = 0;
        if (!ctx_ || !JS_IsObject(v_)) return;
        size_t off = 0, len = 0, bpe = 0;
        JSValue buf = JS_GetTypedArrayBuffer(ctx_, v_, &off, &len, &bpe);
        if (JS_IsException(buf)) { JS_FreeValue(ctx_, JS_GetException(ctx_)); return; }
        size_t total = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx_, &total, buf);
        JS_FreeValue(ctx_, buf);
        if (!base) { JS_FreeValue(ctx_, JS_GetException(ctx_)); return; }
        data_ = base + off;
        byte_len_ = len;
        bpe_ = bpe ? bpe : 1;
    }

    uint8_t *data_ = nullptr;
    size_t byte_len_ = 0;
    size_t bpe_ = 0;
};

class TypedArray : public TypedArrayBase {
public:
    TypedArray(JSContext *ctx, JSValue v) : TypedArrayBase(ctx, v) {}
    template <typename T> T As() const { return T(ctx_, v_); }
};

#define JSGLQ_TYPED_ARRAY(NAME, CTYPE)                                      \
    class NAME : public TypedArrayBase {                                    \
    public:                                                                 \
        NAME(JSContext *ctx, JSValue v) : TypedArrayBase(ctx, v) {}         \
        CTYPE *Data() const { return reinterpret_cast<CTYPE *>(data_); }    \
    }

JSGLQ_TYPED_ARRAY(Uint8Array,   uint8_t);
JSGLQ_TYPED_ARRAY(Int8Array,    int8_t);
JSGLQ_TYPED_ARRAY(Uint16Array,  uint16_t);
JSGLQ_TYPED_ARRAY(Int16Array,   int16_t);
JSGLQ_TYPED_ARRAY(Uint32Array,  uint32_t);
JSGLQ_TYPED_ARRAY(Int32Array,   int32_t);
JSGLQ_TYPED_ARRAY(Float32Array, float);
JSGLQ_TYPED_ARRAY(Float64Array, double);

#undef JSGLQ_TYPED_ARRAY

/* Buffer<T>: Node-specific in origin, used here only for .Data()/.Length(). */
template <typename T>
class Buffer : public TypedArrayBase {
public:
    Buffer(JSContext *ctx, JSValue v) : TypedArrayBase(ctx, v) {}
    T *Data() const { return reinterpret_cast<T *>(data_); }
    size_t Length() const { return bpe_ ? byte_len_ / sizeof(T) : byte_len_ / sizeof(T); }
};

/* ---------------------------------------------------------------- Object ----- */

class Object : public Value {
public:
    Object(JSContext *ctx, JSValue v) : Value(ctx, v) {}
    static Object New(JSContext *ctx) { return Object(ctx, JS_NewObject(ctx)); }

    void Set(const char *key, const Value &val) {
        JS_SetPropertyStr(ctx_, v_, key, JS_DupValue(ctx_, val.raw()));
    }
    void Set(const char *key, JSValue val) { JS_SetPropertyStr(ctx_, v_, key, val); }

    Value Get(const char *key) const {
        return Value(ctx_, JS_GetPropertyStr(ctx_, v_, key));
    }
    bool Has(const char *key) const {
        JSAtom atom = JS_NewAtom(ctx_, key);
        int r = JS_HasProperty(ctx_, v_, atom);
        JS_FreeAtom(ctx_, atom);
        return r > 0;
    }
};

class Array : public Object {
public:
    Array(JSContext *ctx, JSValue v) : Object(ctx, v) {}
    static Array New(JSContext *ctx) { return Array(ctx, JS_NewArray(ctx)); }
    uint32_t Length() const {
        JSValue l = JS_GetPropertyStr(ctx_, v_, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx_, &n, l);
        JS_FreeValue(ctx_, l);
        return n;
    }
    void Set(uint32_t i, JSValue val) { JS_SetPropertyUint32(ctx_, v_, i, val); }
    Value Get(uint32_t i) const { return Value(ctx_, JS_GetPropertyUint32(ctx_, v_, i)); }
};

/* ---------------------------------------------------------- CallbackInfo ----- */

class CallbackInfo {
public:
    CallbackInfo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        : ctx_(ctx), this_(this_val), argc_(argc), argv_(argv) {}

    ::Napi::Env Env() const { return ::Napi::Env(ctx_); }
    size_t Length() const { return (size_t)argc_; }

    /* Out-of-range reads return undefined rather than trapping: the GL bindings
       lean on optional trailing arguments (`gl.foo(a, b)` vs `gl.foo(a, b, c)`). */
    Value operator[](size_t i) const {
        if ((int)i >= argc_) return Value(ctx_, JS_UNDEFINED);
        return Value(ctx_, argv_[i]);
    }

    Value This() const { return Value(ctx_, this_); }

private:
    JSContext *ctx_;
    JSValueConst this_;
    int argc_;
    JSValueConst *argv_;
};

} /* namespace Napi */

#endif /* JSGLQ_NAPI_SHIM_H */
