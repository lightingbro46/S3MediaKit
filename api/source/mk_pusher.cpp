#include <assert.h>
#include "mk_pusher.h"
#include "Pusher/MediaPusher.h"

using namespace toolkit;
using namespace mediakit;

API_EXPORT mk_pusher API_CALL mk_pusher_create(const char *schema,const char *vhost,const char *app, const char *stream){
    assert(schema && vhost && app && schema);
    MediaPusher::Ptr *obj = new MediaPusher::Ptr(new MediaPusher(schema,vhost,app,stream));
    return (mk_pusher)obj;
}

API_EXPORT mk_pusher API_CALL mk_pusher_create_src(mk_media_source ctx){
    assert(ctx);
    MediaSource *src = (MediaSource *)ctx;
    MediaPusher::Ptr *obj = new MediaPusher::Ptr(new MediaPusher(src->shared_from_this()));
    return (mk_pusher)obj;
}

API_EXPORT void API_CALL mk_pusher_release(mk_pusher ctx){
    assert(ctx);
    MediaPusher::Ptr *obj = (MediaPusher::Ptr *)ctx;
    delete obj;
}

API_EXPORT void API_CALL mk_pusher_set_option(mk_pusher ctx, const char *key, const char *val){
    assert(ctx && key && val);
    MediaPusher::Ptr &obj = *((MediaPusher::Ptr *)ctx);
    std::string key_str(key), val_str(val);
    obj->getPoller()->async([obj,key_str,val_str](){
        // Switch threads and then operate
        (*obj)[key_str] = val_str;
    });
}

API_EXPORT void API_CALL mk_pusher_publish(mk_pusher ctx,const char *url){
    assert(ctx && url);
    MediaPusher::Ptr &obj = *((MediaPusher::Ptr *)ctx);
    std::string url_str(url);
    obj->getPoller()->async([obj,url_str](){
        // Switch threads and then operate
        obj->publish(url_str);
    });
}

API_EXPORT void API_CALL mk_pusher_set_on_result(mk_pusher ctx, on_mk_push_event cb, void *user_data){
    mk_pusher_set_on_result2(ctx, cb, user_data, nullptr);
}

API_EXPORT void API_CALL mk_pusher_set_on_result2(mk_pusher ctx, on_mk_push_event cb, void *user_data, on_user_data_free user_data_free) {
    assert(ctx && cb);
    MediaPusher::Ptr &obj = *((MediaPusher::Ptr *)ctx);
    std::shared_ptr<void> ptr(user_data, user_data_free ? user_data_free : [](void *) {});
    obj->getPoller()->async([obj, cb, ptr]() {
        // Switch threads and then operate
        obj->setOnPublished([cb, ptr](const SockException &ex) { cb(ptr.get(), ex.getErrCode(), ex.what()); });
    });
}

API_EXPORT void API_CALL mk_pusher_set_on_shutdown(mk_pusher ctx, on_mk_push_event cb, void *user_data){
    mk_pusher_set_on_shutdown2(ctx, cb, user_data, nullptr);
}

API_EXPORT void API_CALL mk_pusher_set_on_shutdown2(mk_pusher ctx, on_mk_push_event cb, void *user_data, on_user_data_free user_data_free) {
    assert(ctx && cb);
    MediaPusher::Ptr &obj = *((MediaPusher::Ptr *)ctx);
    std::shared_ptr<void> ptr(user_data, user_data_free ? user_data_free : [](void *) {});
    obj->getPoller()->async([obj, cb, ptr]() {
        // Switch threads and then operate
        obj->setOnShutdown([cb, ptr](const SockException &ex) { cb(ptr.get(), ex.getErrCode(), ex.what()); });
    });
}
