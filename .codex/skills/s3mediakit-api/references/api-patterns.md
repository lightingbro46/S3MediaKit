# API patterns

## Synchronous map endpoint

```cpp
api_regist("/media/mserver/example", [](API_ARGS_MAP) {
    CHECK_AUTH_TOKEN();
    CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
    CHECK_ARGS_("id");
    val["data"] = result;
});
```

## Asynchronous endpoint

```cpp
api_regist("/media/mserver/example", [](API_ARGS_MAP_ASYNC) {
    CHECK_AUTH_TOKEN();
    CHECK_ARGS_("id");
    doWork([invoker, headerOut, val]() mutable {
        val["data"] = result;
        invoker(200, headerOut, val.toStyledString());
    });
});
```

Use the actual repository macros and callback signatures; the snippets are structural, not copy-paste guarantees. Never return an async response twice.
