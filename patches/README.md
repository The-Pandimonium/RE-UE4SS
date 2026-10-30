# Patches

`UEPseudo-FName-case-preserving.patch` – local fix for the `deps/first/Unreal` submodule (Re-UE4SS/UEPseudo is private,
so it cannot be forked): the `FName(uint32,uint32,uint32)` constructor must build `FNameEntryId` values on
case-preserving-name builds. Apply with `git -C deps/first/Unreal apply ../../../patches/UEPseudo-FName-case-preserving.patch`.
