# texedit hooks (apply by hand once the sketch job has landed)

`source/texedit.hpp` (class `U2TexEdit`, one instance `TexEd()`) holds all the code for "click a
surface, adjust its texture". It needs 7 small hooks: 4 in `u2shaders.hpp`, 3 kinds in
`d3d8to9_device.cpp`. Anchors are quoted exactly as they are in fork commit 255d621 (gi-cascades, after the sketch job);
line numbers are only a guide. Checked: these hooks applied to a copy of 255d621's source build the
whole project (MSBuild Release|Win32, v145) without errors.

Also install `U2Shaders/tex_adjust.hlsl` into `<game>\System\U2Shaders\` next to the other shaders
(the bake reads it at run time; without it the panel says "U2Shaders\tex_adjust.hlsl not found").

`source/texedit_compiletest.cpp` is a stand-alone compile check (stub host and stage types); it is not
in the project and must stay out of it.

---

## u2shaders.hpp

### H1. include (top, ~l.68)
Anchor:
```cpp
#include "crash.hpp"
```
Insert after it:
```cpp
#include "texedit.hpp"
```

### H2. Replacement: let it look when only texedit has work, and hand every result through it (~l.812-841)
Anchor (the early-out in `IDirect3DTexture9 *Replacement(...)`):
```cpp
		if (Tex == nullptr || (Replacements.empty() && U2Blood::Count == 0 && U2Runs::Count == 0))
			return nullptr;
```
Replace with:
```cpp
		if (Tex == nullptr || (Replacements.empty() && U2Blood::Count == 0 && U2Runs::Count == 0 && !TexEd().Active()))
			return nullptr;
```
Anchor (same function, a few lines further):
```cpp
		const auto It = Replacements.find(Hash);
		if (It == Replacements.end())
			return nullptr;
```
Replace with:
```cpp
		const auto It = Replacements.find(Hash);
		if (It == Replacements.end())
			return TexEd().Adjusted(Dev, Tex, Hash, nullptr);       // texadjust: the game's texture adjusted
```
Anchor (the function's last line, after the `if (!R.Tried) { ... }` block):
```cpp
		return R.Tex;
	}
```
Replace with:
```cpp
		return TexEd().Adjusted(Dev, R.Tex ? R.Tex : Tex, Hash, R.Tex);   // texadjust on top of replace= / texgrade=
	}
```
(Blood pools and wall runs return before this and are never adjusted.)

### H3. GM panel UI: the "Texture" section (GmPanelUi, ~l.3457)
Anchor:
```cpp
		ImGui::SeparatorText("Journal (this map)");
```
Insert before it:
```cpp
		TexEd().PanelUi(*this);              // texedit.hpp: pick a texture, sliders, Save to the journal
```

### H4. GM panel world click: a waiting texture pick takes the click (GmPanelDraw, ~l.3672)
Anchor (inside `if (GmDispW >= 16 && GmDispH >= 16) { ... }`):
```cpp
					GmWorldClick();
```
Replace with:
```cpp
					if (!TexEd().WorldClick())   // texedit: a pick waiting for its click (Esc cancels)
						GmWorldClick();
```

---

## d3d8to9_device.cpp

### D1. Reset (~l.235)
Anchor:
```cpp
	U2.OnLost();
```
Insert after it:
```cpp
	TexEd().OnLost();                    // texedit: its render targets (bakes, pick target) are DEFAULT pool
```

### D2. Present (~l.283)
Anchor:
```cpp
	U2.OnPresent(ProxyInterface);
```
Insert BEFORE it (the panel, drawn inside U2.OnPresent, then shows this frame's bakes):
```cpp
	TexEd().OnPresent(U2, ProxyInterface);   // texedit: pick readback, ini/journal watch, adjust bakes
```

### D3. The four draw calls: the pick pass (after the shotmask redraw block)
In each of `DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP` the
anchor is the shotmask block's end followed by `return D3D_OK;`:
```cpp
		U2.MaskEnd(ProxyInterface);
	}
	return D3D_OK;
```
Insert between `}` and `return D3D_OK;` the matching redraw:

DrawPrimitive:
```cpp
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		TexEd().PickEnd(ProxyInterface);
	}
```
DrawIndexedPrimitive:
```cpp
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawIndexedPrimitive(PrimitiveType, CurrentBaseVertexIndex, MinIndex, NumVertices, StartIndex, PrimitiveCount);
		TexEd().PickEnd(ProxyInterface);
	}
```
DrawPrimitiveUP:
```cpp
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
		TexEd().PickEnd(ProxyInterface);
	}
```
DrawIndexedPrimitiveUP:
```cpp
	if (TexEd().PickBegin(U2, ProxyInterface, U2Stages))   // texedit: the click frame's pick pass
	{
		ProxyInterface->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
		TexEd().PickEnd(ProxyInterface);
	}
```
`PickBegin` returns false at once except on the one pick frame (`PickState == Drawing`), so this
costs a compare per draw otherwise.

---

## What the host must provide (already there today)
`U2Shaders` members used by the templates: `Dir`, `CurMap`, `Known(IDirect3DTexture9*, DWORD&)`,
`GmSend(const char*, ...)` (all public). Stage type: `Direct3DTexture8` (`GetProxyInterface()`,
`U2Hash`). U2GM must have the `gm tex` command (GMMaster.uc, added with this change).
