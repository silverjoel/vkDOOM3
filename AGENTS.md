# vkDOOM3 x64 Migration Instructions

## Current task

The current project phase is conversion of vkDOOM3 to x64-only Windows builds.

Work only on the x64 conversion until this phase is complete.

The target configurations are:

- Debug | x64
- Release | x64

Win32/x86 support is being intentionally removed.

## Important boundaries

Do NOT begin any later modernization phase unless explicitly instructed.

Specifically, do not yet:

- convert the renderer to Vulkan-only
- remove DirectSound
- redesign the OpenAL/EFX implementation
- replace DirectInput with Raw Input
- remove the June 2010 DirectX SDK
- begin the RmlUi migration

Those are later project phases.

## Windows macros

Do not remove or replace `_WIN32` simply because the build is 64-bit.

Microsoft defines `_WIN32` for both 32-bit and 64-bit Windows builds.

Use `_WIN64` only when code genuinely requires a 64-bit-Windows distinction.

Remove `_USE_32BIT_TIME_T` from x64 configuration/code.

## x64 correctness

Audit carefully for assumptions that pointers, handles, offsets, sizes, or addresses fit in 32-bit integers.

Prefer the semantically correct types where applicable:

- size_t for object/buffer sizes
- ptrdiff_t for pointer differences
- intptr_t / uintptr_t for integers that must hold pointers
- Windows pointer-sized types such as INT_PTR, UINT_PTR, LONG_PTR,
  ULONG_PTR, DWORD_PTR, WPARAM and LPARAM where appropriate
- intptr_t for CRT handles such as _findfirst/_findnext handles

Do not mechanically replace every int with size_t or intptr_t.

Do not fix x64 warnings merely by adding unsafe casts.

Every narrowing conversion must be examined to determine whether the value is
actually guaranteed to fit in the destination type.

## Compatibility

Binary file formats, network protocols, save-game formats, asset formats and
serialized structures must NOT silently change merely because the executable
is now 64-bit.

Do not change serialized 32-bit fields into 64-bit fields unless the format
itself requires that change.

## Compiler warnings

Do not globally disable warnings to make the x64 build succeed.

Fix legitimate warnings at their source.

In particular, investigate:

- pointer/integer truncation
- size_t to int conversion
- pointer arithmetic
- handle truncation
- x86-specific inline assembly
- x86-only compiler intrinsics
- format-string type mismatches
- assumptions about sizeof(long), sizeof(void *), or sizeof(time_t)

## Project configuration

Convert the Visual Studio projects/solution to x64-only.

Remove Win32 configurations after confirming their x64 equivalents are correct.

Remove obsolete x86 library paths and x86-specific library selections.

Preserve required x64 Vulkan, OpenAL, XInput and current runtime dependencies.

Do not remove a dependency merely because it is planned for removal in a
later modernization phase.

## Verification

Make changes in small logical batches.

After each meaningful batch:

1. Build Debug | x64.
2. Resolve the first meaningful compiler/linker failure.
3. Rebuild.
4. Continue until Debug | x64 is clean.

Then repeat for Release | x64.

Do not hide build failures.

If a failure has several plausible fixes with different behavioral
consequences, stop and explain the alternatives rather than guessing.

## Git

Keep changes focused.

Do not make unrelated formatting changes.

Do not rewrite large files unnecessarily.

Prefer small reviewable commits organized by subsystem or problem type.