#include "node.h"
#include "node_version.h"
#include "nan.h"
#include <string>
using namespace v8;
using namespace node;

class ContextWrap;

class ContextifyContext : public ObjectWrap {
public:
    Nan::Persistent<Context> context;
    Nan::Persistent<Object>  sandbox;
    Nan::Persistent<Object>  proxyGlobal;

    static Nan::Persistent<FunctionTemplate> jsTmpl;

    ContextifyContext(Local<Object> sbox) {
        Nan::HandleScope scope;
        sandbox.Reset(sbox);
    }

    ~ContextifyContext() {
        context.Reset();
        proxyGlobal.Reset();
        sandbox.Reset();

        // Provide a GC hint that the context has gone away. Without this call it
        // does not seem that the collector will touch the context until under extreme
        // stress.
        Nan::ContextDisposedNotification();
    }

    // We override ObjectWrap::Wrap so that we can create our context after
    // we have a reference to our "host" JavaScript object.  If we try to use
    // handle_ in the ContextifyContext constructor, it will be empty since it's
    // set in ObjectWrap::Wrap.
    void Wrap(Handle<Object> handle);

    static void Init(Handle<Object> target) {
        Nan::HandleScope scope;

        Local<String> className = Nan::New("ContextifyContext").ToLocalChecked();

        Local<FunctionTemplate> ljsTmpl = Nan::New<FunctionTemplate>(New);
        ljsTmpl->InstanceTemplate()->SetInternalFieldCount(1);
        ljsTmpl->SetClassName(className);
        Nan::SetPrototypeMethod(ljsTmpl, "run",       ContextifyContext::Run);
        Nan::SetPrototypeMethod(ljsTmpl, "getGlobal", ContextifyContext::GetGlobal);

        jsTmpl.Reset(ljsTmpl);
        target->Set(className, ljsTmpl->GetFunction());
    }

    static NAN_METHOD(New) {
        if (info.Length() < 1) {
            Nan::ThrowError("Wrong number of arguments passed to ContextifyContext constructor");
            info.GetReturnValue().SetUndefined();
            return;
        }

        if (!info[0]->IsObject()) {
            Nan::ThrowTypeError("Argument to ContextifyContext constructor must be an object.");
            info.GetReturnValue().SetUndefined();
            return;
        }

        ContextifyContext* ctx = new ContextifyContext(info[0]->ToObject());
        ctx->Wrap(info.This());
        info.GetReturnValue().Set(info.This());
    }

    static NAN_METHOD(Run) {
        if (info.Length() == 0) {
            Nan::ThrowError("Must supply at least 1 argument to run");
            info.GetReturnValue().SetUndefined();
            return;
        }
        if (!info[0]->IsString()) {
            Nan::ThrowTypeError("First argument to run must be a String.");
            info.GetReturnValue().SetUndefined();
            return;
        }
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextifyContext>(info.This());
        Local<Context> lcontext = Nan::New(ctx->context);
        Context::Scope ctxScope(lcontext);
        Local<String> code = info[0]->ToString();

        TryCatch trycatch;
        MaybeLocal<Nan::BoundScript> script;

        if (info.Length() > 1 && info[1]->IsString()) {
            ScriptOrigin origin(info[1]->ToString());
            script = Nan::CompileScript(code, origin);
        } else {
            script = Nan::CompileScript(code);
        }

        if (script.IsEmpty()) {
            info.GetReturnValue().Set(trycatch.ReThrow());
            return;
        }

        MaybeLocal<Value> result = Nan::RunScript(script.ToLocalChecked());

        if (result.IsEmpty()) {
            info.GetReturnValue().Set(trycatch.ReThrow());
        } else {
            info.GetReturnValue().Set(result.ToLocalChecked());
        }
    }

    static bool InstanceOf(Handle<Value> value) {
        return Nan::New(jsTmpl)->HasInstance(value);
    }

    static NAN_METHOD(GetGlobal) {
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextifyContext>(info.This());
        info.GetReturnValue().Set(Nan::New(ctx->proxyGlobal));
    }
};

// This is an object that just keeps an internal pointer to this
// ContextifyContext.  It's passed to the NamedPropertyHandler.  If we
// pass the main JavaScript context object we're embedded in, then the
// NamedPropertyHandler will store a reference to it forever and keep it
// from getting gc'd.
class ContextWrap : public ObjectWrap {
public:
    static void Init(void) {
        Nan::HandleScope scope;

        Local<FunctionTemplate> tmpl = Nan::New<FunctionTemplate>();
        tmpl->InstanceTemplate()->SetInternalFieldCount(1);
        functionTemplate.Reset(tmpl);
        constructor.Reset(tmpl->GetFunction());
    }

    static Local<Context> createV8Context(Handle<Object> jsContextify) {
        Nan::EscapableHandleScope scope;
        Local<Object> wrapper = Nan::New(constructor)->NewInstance();

        ContextWrap *contextWrapper = new ContextWrap();
        contextWrapper->Wrap(wrapper);

        Local<Object> obj = Local<Object>::New(Isolate::GetCurrent(), jsContextify);
        Nan::Persistent<Object>(obj).SetWeak(contextWrapper, &ContextWrap::weakCallback, Nan::WeakCallbackType::kParameter);
        contextWrapper->ctx = ObjectWrap::Unwrap<ContextifyContext>(jsContextify);

        Local<FunctionTemplate> ftmpl = Nan::New<FunctionTemplate>();
        ftmpl->SetHiddenPrototype(true);
        ftmpl->SetClassName(Nan::New(contextWrapper->ctx->sandbox)->GetConstructorName());
        Local<ObjectTemplate> otmpl = ftmpl->InstanceTemplate();
        otmpl->SetNamedPropertyHandler(GlobalPropertyGetter,
                                       GlobalPropertySetter,
                                       GlobalPropertyQuery,
                                       GlobalPropertyDeleter,
                                       GlobalPropertyEnumerator,
                                       wrapper);
        otmpl->SetAccessCheckCallbacks(GlobalPropertyNamedAccessCheck,
                                       GlobalPropertyIndexedAccessCheck);
        return scope.Escape(Nan::New<Context>(static_cast<ExtensionConfiguration*>(NULL), otmpl));
    }

private:
    ContextWrap() :ctx(NULL) {}

    ~ContextWrap() {
    }

    static bool GlobalPropertyNamedAccessCheck(Local<Object> host,
                                               Local<Value>  key,
                                               AccessType    type,
                                               Local<Value>  data) {
        return true;
    }

    static bool GlobalPropertyIndexedAccessCheck(Local<Object> host,
                                                 uint32_t      key,
                                                 AccessType    type,
                                                 Local<Value>  data) {
        return true;
    }

    static void GlobalPropertyGetter(Local<String> property, const PropertyCallbackInfo<Value>& info) {
        Local<Object> data = info.Data()->ToObject();
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextWrap>(data)->ctx;

        if (!ctx) {
            info.GetReturnValue().SetUndefined();
            return;
        }

        Local<Value> rv = Nan::New(ctx->sandbox)->GetRealNamedProperty(property);

//        if (rv.IsEmpty()) {
//            rv = Nan::New(ctx->proxyGlobal)->GetRealNamedProperty(property);
//        }

        info.GetReturnValue().Set(rv);
    }

    static void GlobalPropertySetter(Local<String> property, Local<Value> value, const PropertyCallbackInfo<Value>& info) {
        Local<Object> data = info.Data()->ToObject();
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextWrap>(data)->ctx;

        if (!ctx) {
            info.GetReturnValue().SetUndefined();
            return;
        }

        Nan::New(ctx->sandbox)->Set(property, value);
        info.GetReturnValue().Set(value);
    }

    static void GlobalPropertyQuery(Local<String> property, const PropertyCallbackInfo<Integer>& info) {
        Local<Object> data = info.Data()->ToObject();
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextWrap>(data)->ctx;

        if (!ctx) {
            info.GetReturnValue().Set(Nan::New<Integer>(None));
            return;
        }

        if (!Nan::New(ctx->sandbox)->GetRealNamedProperty(property).IsEmpty() ||
            !Nan::New(ctx->proxyGlobal)->GetRealNamedProperty(property).IsEmpty()) {
            info.GetReturnValue().Set(Nan::New<Integer>(None));
         } else {
            info.GetReturnValue().Set(Handle<Integer>());
         }
    }

    static void GlobalPropertyDeleter(Local<String> property, const PropertyCallbackInfo<Boolean>& info) {
        Local<Object> data = info.Data()->ToObject();
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextWrap>(data)->ctx;

        if (!ctx) {
            info.GetReturnValue().Set(Nan::New(false));
            return;
        }

        bool success = Nan::New(ctx->sandbox)->Delete(property);
        info.GetReturnValue().Set(Nan::New(success));
    }

    static void GlobalPropertyEnumerator(const PropertyCallbackInfo<Array>& info) {
        Local<Object> data = info.Data()->ToObject();
        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextWrap>(data)->ctx;

        if (!ctx) {
            Local<Array> blank = Array::New(0);
            info.GetReturnValue().Set(blank);
            return;
        }

        info.GetReturnValue().Set(Nan::New(ctx->sandbox)->GetPropertyNames());
    }

    static void weakCallback(const Nan::WeakCallbackInfo<ContextWrap>& data) {
        ContextWrap *self = data.GetParameter();
        self->ctx = NULL;
    }

    static Nan::Persistent<FunctionTemplate> functionTemplate;
    static Nan::Persistent<Function>         constructor;
    ContextifyContext                   *ctx;
};

Nan::Persistent<FunctionTemplate> ContextWrap::functionTemplate;
Nan::Persistent<Function>         ContextWrap::constructor;

void ContextifyContext::Wrap(Handle<Object> handle) {
    ObjectWrap::Wrap(handle);
    Local<Context> lcontext = ContextWrap::createV8Context(handle);
    context.Reset(lcontext);
    proxyGlobal.Reset(lcontext->Global());
}

class ContextifyScript : public ObjectWrap {
public:
    static Nan::Persistent<FunctionTemplate> scriptTmpl;
    Nan::Persistent<Nan::UnboundScript> script;

    static void Init(Handle<Object> target) {
        Nan::HandleScope scope;

        Local<String> className = Nan::New("ContextifyScript").ToLocalChecked();

        Local<FunctionTemplate> lscriptTmpl = Nan::New<FunctionTemplate>(New);
        lscriptTmpl->InstanceTemplate()->SetInternalFieldCount(1);
        lscriptTmpl->SetClassName(className);

        Nan::SetPrototypeMethod(lscriptTmpl, "runInContext", RunInContext);

        scriptTmpl.Reset(lscriptTmpl);
        target->Set(className, lscriptTmpl->GetFunction());
    }
    static NAN_METHOD(New) {
        ContextifyScript *contextify_script = new ContextifyScript();
        contextify_script->Wrap(info.Holder());

        if (info.Length() < 1) {
            Nan::ThrowTypeError("needs at least 'code' argument.");
            info.GetReturnValue().SetUndefined();
            return;
        }

        Local<String> code = info[0]->ToString();

        TryCatch trycatch;

        MaybeLocal<String> filename = info.Length() > 1
                               ? info[1]->ToString()
                               : Nan::New("ContextifyScript.<anonymous>");

        if (filename.IsEmpty()) {
            info.GetReturnValue().Set(trycatch.ReThrow());
            return;
        }

        Handle<Context> context = Nan::GetCurrentContext();
        Context::Scope context_scope(context);

        ScriptOrigin origin(filename.ToLocalChecked());
        MaybeLocal<Nan::UnboundScript> v8_script = Nan::New<Nan::UnboundScript>(code, origin);

        if (v8_script.IsEmpty()) {
            info.GetReturnValue().Set(trycatch.ReThrow());
            return;
        }

        contextify_script->script.Reset(v8_script.ToLocalChecked());

        info.GetReturnValue().Set(info.This());
    }

    static NAN_METHOD(RunInContext) {
        if (info.Length() == 0) {
            Nan::ThrowError("Must supply at least 1 argument to runInContext");
            info.GetReturnValue().SetUndefined();
            return;
        }
        if (!ContextifyContext::InstanceOf(info[0]->ToObject())) {
            Nan::ThrowTypeError("First argument must be a ContextifyContext.");
            info.GetReturnValue().SetUndefined();
            return;
        }

        ContextifyContext* ctx = ObjectWrap::Unwrap<ContextifyContext>(info[0]->ToObject());
        Local<Context> lcontext = Nan::New(ctx->context);

        Context::Scope scope(lcontext);

        ContextifyScript* wrapped_script = ObjectWrap::Unwrap<ContextifyScript>(info.This());
        MaybeLocal<Nan::UnboundScript> script = Nan::New(wrapped_script->script);

        TryCatch trycatch;

        if (script.IsEmpty()) {
            info.GetReturnValue().Set(trycatch.ReThrow());
            return;
        }

        MaybeLocal<Value> result = Nan::RunScript(script.ToLocalChecked());

        if (result.IsEmpty()) {
            info.GetReturnValue().Set(trycatch.ReThrow());
        } else {
            info.GetReturnValue().Set(result.ToLocalChecked());
        }
    }

    ~ContextifyScript() {
        script.Reset();
    }
};

Nan::Persistent<FunctionTemplate> ContextifyContext::jsTmpl;
Nan::Persistent<FunctionTemplate> ContextifyScript::scriptTmpl;

extern "C" {
    static void init(Handle<Object> target) {
        ContextifyContext::Init(target);
        ContextifyScript::Init(target);
        ContextWrap::Init();
    }
    NODE_MODULE(contextify, init)
};
