#pragma once

#include <assert.h>
#include <js.h>
#include <stdint.h>
#include <string>
#include <unordered_map>

#include <winrt/base.h>

#include <winrt/Windows.Foundation.h>

static const js_type_tag_t bare_winrt__carrier = {0xee17b960c95a11de, 0x1bc966f0c1330207};

struct bare_winrt_entry_t {
  winrt::Windows::Foundation::IInspectable object;
  js_ref_t *wrapper;
  uint32_t claims;
};

struct bare_winrt__claim_t {
  uint32_t tag;
  js_ref_t *wrapper;
};

typedef struct {
  std::unordered_map<uint32_t, bare_winrt_entry_t> entries;
  std::unordered_map<void *, uint32_t> tags;

  uint32_t next_tag;
  uint32_t refs;
} bare_winrt_registry_t;

/**
 * Keep `registry` alive, for example while native code holds on to it. Give the
 * reference back with `bare_winrt_registry_release()`.
 */
static void
bare_winrt_registry_retain(bare_winrt_registry_t *registry) {
  registry->refs++;
}

/**
 * Give back a reference to `registry`. The registry is freed when the last
 * reference is gone. Objects that were tagged but never claimed are released
 * here, while the Windows App SDK is still running. At exit it would be too
 * late.
 */
static void
bare_winrt_registry_release(bare_winrt_registry_t *registry) {
  assert(registry->refs > 0);

  if (--registry->refs > 0) return;

  delete registry;
}

static void
bare_winrt__on_registry_release(js_env_t *env, void *data, void *finalize_hint) {
  bare_winrt_registry_release(static_cast<bare_winrt_registry_t *>(data));
}

/**
 * Create a registry for an addon, and pass it as the data pointer of every
 * function the addon exports. The registry lives as long as `exports` and every
 * token it has handed out.
 */
static bare_winrt_registry_t *
bare_winrt_registry_create(js_env_t *env, js_value_t *exports) {
  int err;

  auto registry = new bare_winrt_registry_t();

  registry->next_tag = 1;
  registry->refs = 1;

  err = js_add_finalizer(env, exports, registry, bare_winrt__on_registry_release, nullptr, nullptr);
  assert(err == 0);

  return registry;
}

// COM identity is the `IUnknown` pointer, not the pointer of the interface held.
static void *
bare_winrt__identity(winrt::Windows::Foundation::IInspectable const &object) {
  return winrt::get_abi(object.as<winrt::Windows::Foundation::IUnknown>());
}

/**
 * Give `object` a tag and return it, or 0 if `object` is `nullptr`. An object
 * that already has a tag keeps it, whichever of its interfaces is passed.
 */
static uint32_t
bare_winrt_tag(bare_winrt_registry_t *registry, winrt::Windows::Foundation::IInspectable const &object) {
  if (object == nullptr) return 0;

  auto identity = bare_winrt__identity(object);

  auto existing = registry->tags.find(identity);

  if (existing != registry->tags.end()) return existing->second;

  uint32_t tag = registry->next_tag++;

  registry->entries.emplace(tag, bare_winrt_entry_t{object, nullptr, 0});
  registry->tags.emplace(identity, tag);

  return tag;
}

static bare_winrt_entry_t *
bare_winrt__entry(bare_winrt_registry_t *registry, uint32_t tag) {
  auto entry = registry->entries.find(tag);

  if (entry == registry->entries.end()) return nullptr;

  return &entry->second;
}

static js_value_t *
bare_winrt__wrapper(js_env_t *env, bare_winrt_entry_t *entry) {
  if (entry->wrapper == nullptr) return nullptr;

  js_value_t *result;
  int err = js_get_reference_value(env, entry->wrapper, &result);
  assert(err == 0);

  return result;
}

/**
 * Return the object for `tag`, or `nullptr` if there is none.
 */
static winrt::Windows::Foundation::IInspectable
bare_winrt_object(bare_winrt_registry_t *registry, uint32_t tag) {
  auto entry = bare_winrt__entry(registry, tag);

  if (entry == nullptr) return nullptr;

  return entry->object;
}

static int
bare_winrt__read_uint32(js_env_t *env, js_value_t *value, const char *name, uint32_t *result) {
  int err;

  bool is;
  err = js_is_number(env, value, &is);
  assert(err == 0);

  if (!is) {
    err = js_throw_type_errorf(env, nullptr, "Expected '%s' to be a number", name);
    assert(err == 0);

    return -1;
  }

  err = js_get_value_uint32(env, value, result);
  assert(err == 0);

  return 0;
}

/**
 * Read the object for the tag in `value` into `result` and return 0. If `value`
 * is not a number or not a known tag, throw a JavaScript error that mentions
 * `name` and return -1.
 */
static int
bare_winrt_read_tag(js_env_t *env, bare_winrt_registry_t *registry, js_value_t *value, const char *name, winrt::Windows::Foundation::IInspectable *result) {
  int err;

  uint32_t tag;
  err = bare_winrt__read_uint32(env, value, name, &tag);
  if (err < 0) return err;

  auto entry = bare_winrt__entry(registry, tag);

  if (entry == nullptr) {
    err = js_throw_errorf(env, nullptr, "Unknown tag %u", tag);
    assert(err == 0);

    return -1;
  }

  *result = entry->object;

  return 0;
}

/**
 * Like `bare_winrt_read_tag()`, but read the object as a `T`. If the object is
 * not a `T`, throw a `TypeError` and return -1.
 */
template <typename T>
static int
bare_winrt_read_type(js_env_t *env, bare_winrt_registry_t *registry, js_value_t *value, const char *name, T *result) {
  int err;

  winrt::Windows::Foundation::IInspectable object;
  err = bare_winrt_read_tag(env, registry, value, name, &object);
  if (err < 0) return err;

  auto typed = object.try_as<T>();

  if (typed == nullptr) {
    auto expected = winrt::to_string(winrt::name_of<T>());
    auto actual = winrt::to_string(winrt::get_class_name(object));

    err = js_throw_type_errorf(env, nullptr, "Expected '%s' to be a %s, not a %s", name, expected.c_str(), actual.c_str());
    assert(err == 0);

    return -1;
  }

  *result = typed;

  return 0;
}

/**
 * Return the JavaScript wrapper of `object`, or `nullptr` if it has none.
 */
static js_value_t *
bare_winrt_lookup(js_env_t *env, bare_winrt_registry_t *registry, winrt::Windows::Foundation::IInspectable const &object) {
  auto tag = registry->tags.find(bare_winrt__identity(object));

  if (tag == registry->tags.end()) return nullptr;

  auto entry = bare_winrt__entry(registry, tag->second);

  if (entry == nullptr) return nullptr;

  return bare_winrt__wrapper(env, entry);
}

static void
bare_winrt__on_token_finalize(js_env_t *env, void *data, void *finalize_hint) {
  int err;

  auto registry = static_cast<bare_winrt_registry_t *>(finalize_hint);

  auto claim = static_cast<bare_winrt__claim_t *>(data);

  uint32_t tag = claim->tag;

  err = js_delete_reference(env, claim->wrapper);
  assert(err == 0);

  auto entry = bare_winrt__entry(registry, tag);

  if (entry->wrapper == claim->wrapper) entry->wrapper = nullptr;

  delete claim;

  if (--entry->claims == 0) {
    registry->tags.erase(bare_winrt__identity(entry->object));
    registry->entries.erase(tag);
  }

  bare_winrt_registry_release(registry);
}

/**
 * Export as `claim(tag, wrapper)`. Return a token that keeps the object alive
 * until the token is garbage collected. A tag can be claimed more than once,
 * and the object stays alive until every token is gone. The registry does not
 * keep any wrapper alive.
 */
static js_value_t *
bare_winrt_claim(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  bare_winrt_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, nullptr, (void **) &registry);
  assert(err == 0);

  assert(argc == 2);

  uint32_t tag;
  err = bare_winrt__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return nullptr;

  auto entry = bare_winrt__entry(registry, tag);

  if (entry == nullptr) {
    err = js_throw_errorf(env, nullptr, "Unknown tag %u", tag);
    assert(err == 0);

    return nullptr;
  }

  bare_winrt__claim_t *claim = new bare_winrt__claim_t();

  claim->tag = tag;

  err = js_create_reference(env, argv[1], 0, &claim->wrapper);
  assert(err == 0);

  if (bare_winrt__wrapper(env, entry) == nullptr) entry->wrapper = claim->wrapper;

  entry->claims++;

  bare_winrt_registry_retain(registry);

  js_value_t *token;
  err = js_create_external(env, claim, bare_winrt__on_token_finalize, registry, &token);
  assert(err == 0);

  return token;
}

/**
 * Export as `wrapper(tag)`. Return the first wrapper of `tag` that is still
 * alive, or `null` if there is none. An unknown tag is not an error, because
 * `adopt()` asks with a tag that may belong to another addon to find out
 * whether an object is ours.
 */
static js_value_t *
bare_winrt_wrapper(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_winrt_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, nullptr, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  uint32_t tag;
  err = bare_winrt__read_uint32(env, argv[0], "tag", &tag);
  if (err < 0) return nullptr;

  auto *entry = bare_winrt__entry(registry, tag);

  js_value_t *result = entry == nullptr ? nullptr : bare_winrt__wrapper(env, entry);

  if (result == nullptr) {
    err = js_get_null(env, &result);
    assert(err == 0);
  }

  return result;
}

/**
 * Export as `registrySize()`. Return the number of objects in the registry.
 * Useful for finding leaks in tests.
 */
static js_value_t *
bare_winrt_registry_size(js_env_t *env, js_callback_info_t *info) {
  int err;

  bare_winrt_registry_t *registry;
  err = js_get_callback_info(env, info, nullptr, nullptr, nullptr, (void **) &registry);
  assert(err == 0);

  js_value_t *result;
  err = js_create_uint32(env, (uint32_t) registry->entries.size(), &result);
  assert(err == 0);

  return result;
}

static void
bare_winrt__on_carrier_finalize(js_env_t *env, void *data, void *finalize_hint) {
  winrt::Windows::Foundation::IInspectable object = nullptr;

  winrt::attach_abi(object, data);
}

/**
 * Export as `handle(tag)`. Return a handle that another addon can adopt. The
 * handle holds its own reference to the object.
 */
static js_value_t *
bare_winrt_handle(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_winrt_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, nullptr, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  winrt::Windows::Foundation::IInspectable object;
  err = bare_winrt_read_tag(env, registry, argv[0], "tag", &object);
  if (err < 0) return nullptr;

  js_value_t *carrier;
  err = js_create_object(env, &carrier);
  assert(err == 0);

  err = js_wrap(env, carrier, winrt::detach_abi(object), bare_winrt__on_carrier_finalize, nullptr, nullptr);
  assert(err == 0);

  err = js_add_type_tag(env, carrier, &bare_winrt__carrier);
  assert(err == 0);

  return carrier;
}

/**
 * Export as `adopt(handle)`. Return a tag for the object in `handle`. A object
 * that already has a tag keeps it.
 */
static js_value_t *
bare_winrt_adopt(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  bare_winrt_registry_t *registry;
  err = js_get_callback_info(env, info, &argc, argv, nullptr, (void **) &registry);
  assert(err == 0);

  assert(argc == 1);

  bool is;
  err = js_is_object(env, argv[0], &is);
  assert(err == 0);

  if (is) {
    err = js_check_type_tag(env, argv[0], &bare_winrt__carrier, &is);
    assert(err == 0);
  }

  if (!is) {
    err = js_throw_type_error(env, nullptr, "Expected 'carrier' to be a WinRT handle");
    assert(err == 0);

    return nullptr;
  }

  void *abi;
  err = js_unwrap(env, argv[0], &abi);
  assert(err == 0);

  winrt::Windows::Foundation::IInspectable object = nullptr;

  winrt::copy_from_abi(object, abi);

  js_value_t *result;
  err = js_create_uint32(env, bare_winrt_tag(registry, object), &result);
  assert(err == 0);

  return result;
}
