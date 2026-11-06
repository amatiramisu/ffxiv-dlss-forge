#!/usr/bin/env python3
"""Source-only regressions for NR reset predicates and the SDR transfer expression.

Interprets the scalar expressions from the production sources; requires no build or GPU.
"""
from copy import deepcopy
from pathlib import Path
import math
import re
from types import SimpleNamespace as NS
import unittest

SRC = Path(__file__).resolve().parents[1] / "src"
BRIDGE = (SRC / "nr_bridge.cpp").read_text()
SHADER = (SRC / "nr_shaders.hlsl").read_text()


def predicate(expression):
    expression = expression.replace("||", " or ").replace("&&", " and ")
    expression = re.sub(r"!(?!=)", " not ", expression)
    return compile(expression.replace("\n", " ").strip(), "production predicate", "eval")


context_expression = re.search(r"bool chain_reset = (.*?);", BRIDGE, re.S).group(1)
context_expression = context_expression.replace(
    "std::memcmp(mask_state, g.history_mask, sizeof(mask_state))", "mask_difference")
CONTEXT = predicate(context_expression)
pass_expressions = re.findall(r"chain_reset = (chain_reset \|\| pf.reset \|\| active != pf.evaluated);", BRIDGE)
assert len(pass_expressions) == 2, "Both model backends must propagate the same resets"
PASS = predicate(pass_expressions[0])
SDR = re.search(r"if \(passthrough\(\)\)\s*\{[^}]*upgraded = (.*?);", SHADER, re.S).group(1)
SDR = compile(SDR, "production SDR expression", "eval")


def evaluate(code, **values):
    return eval(code, {"__builtins__": {}}, values)


def baseline():
    f = NS(reset=False, source=1, ctx=1, x=0, y=0, w=3840, h=2160,
           guide_w=3840, guide_h=2160, motion_w=3840, motion_h=2160,
           depth_x=0, depth_y=0, motion_x=0, motion_y=0,
           jitter_x=0, jitter_y=0, motion_jittered=False,
           mv_scale_x=1, mv_scale_y=1, depth_inverted=True, hdr=False)
    return dict(f=f, previous=deepcopy(f), previous_valid=True, present=11, estimate=False,
                g=NS(history_present=10, history_mask_on=False, history_estimate=False, history_blend=0,
                     probe_revision=0, history_probe_revision=0, history_codec=True),
                c=NS(codec=True),
                mask_on=False, blend=0, mask_difference=0)


class History(unittest.TestCase):
    def test_codec_switch_resets(self):
        v = baseline()
        v["c"] = NS(codec=False)
        self.assertTrue(evaluate(CONTEXT, **v))

    def test_continuous_and_same_present(self):
        v = baseline()
        self.assertFalse(evaluate(CONTEXT, **v))
        v["present"] = 10
        self.assertFalse(evaluate(CONTEXT, **v))

    def test_every_frame_contract_change(self):
        for field in vars(baseline()["f"]):
            if field in ("jitter_x", "jitter_y"):
                continue
            with self.subTest(field=field):
                v = baseline()
                old = getattr(v["f"], field)
                setattr(v["f"], field, not old if isinstance(old, bool) else old + 1)
                self.assertTrue(evaluate(CONTEXT, **v))

    def test_raster_jitter_sequence_preserves_history(self):
        v = baseline()
        v["f"].jitter_x = .25
        v["f"].jitter_y = -.25
        self.assertFalse(evaluate(CONTEXT, **v))

    def test_interrupted_and_failed_frames(self):
        v = baseline()
        v["present"] += 1
        self.assertTrue(evaluate(CONTEXT, **v))
        v = baseline()
        v["previous_valid"] = False
        self.assertTrue(evaluate(CONTEXT, **v))
        v = baseline()
        v["estimate"] = True
        self.assertTrue(evaluate(CONTEXT, **v))

    def test_masks_and_display_only_changes(self):
        v = baseline()
        v["mask_on"] = True
        self.assertTrue(evaluate(CONTEXT, **v))
        v["g"].history_mask_on = True
        self.assertFalse(evaluate(CONTEXT, **v))
        for key in ("blend", "mask_difference"):
            changed = deepcopy(v)
            changed[key] = 1
            self.assertTrue(evaluate(CONTEXT, **changed))
        v["g"].probe_revision += 1
        self.assertTrue(evaluate(CONTEXT, **v))
        # Composition/debug controls are deliberately outside the model history key.
        for name in ("c.detail", "c.colour", "c.guard", "c.compare", "c.debug_view"):
            self.assertNotIn(name, context_expression)

    def test_reset_propagates_only_downstream(self):
        for changed in range(3):
            for kind in ("rebuild", "live"):
                chain_reset = False
                actual = []
                for i in range(3):
                    pf = NS(reset=kind == "rebuild" and i == changed, evaluated=0)
                    active = int(kind == "live" and i == changed)
                    chain_reset = evaluate(PASS, chain_reset=chain_reset, pf=pf, active=active)
                    actual.append(chain_reset)
                self.assertEqual(actual, [i >= changed for i in range(3)])
        self.assertFalse(evaluate(PASS, chain_reset=False, pf=NS(reset=False, evaluated=1), active=1))


class GuestMode(unittest.TestCase):
    def test_nr_bypass_and_release(self):
        hook = (SRC / "ngx_hook.cpp").read_text()
        entry = hook.split("void run_nr(", 1)[1]
        condition, body = re.search(r"if \((.*?)\)\s*\{(.*?)\n\t\t\}", entry, re.S).groups()
        bypass = predicate(condition)
        for enabled in (False, True):
            for guest_mode in (False, True):
                for nr_enabled in (False, True):
                    s = NS(enabled=enabled, guest_mode=guest_mode, nr=NS(enabled=nr_enabled))
                    self.assertEqual(evaluate(bypass, s=s),
                                     not enabled or guest_mode or not nr_enabled)
        self.assertIn("forge::nr::release();", body)
        self.assertIn("g_nr_on = false;", body)
        self.assertIn("return;", body)

    def test_persistence_and_separate_dlss_generation(self):
        addon = (SRC / "addon.cpp").read_text()
        self.assertIn('s.guest_mode = read_int("GuestMode", s.guest_mode ? 1 : 0) != 0;', addon)
        self.assertIn('section, "GuestMode", s.guest_mode ? 1 : 0', addon)
        self.assertIn('ImGui::Checkbox("Guest mode", &s.guest_mode)', addon)
        self.assertIn("ImGui::BeginDisabled(!s.enabled || s.guest_mode);", addon)
        settings = (SRC / "settings.cpp").read_text()
        recreate = settings.split("const bool recreate =", 1)[1].split(";", 1)[0]
        self.assertNotIn("guest_mode", recreate)


class SDRTransfer(unittest.TestCase):
    def transfer(self, original, model, proxy, detail=1):
        return evaluate(SDR, original=original, model=model, proxy=proxy, detail=detail,
                        lerp=lambda a, b, t: a + (b - a) * t,
                        saturate=lambda v: max(0, min(v, 1)))

    def test_valid_black_and_near_black(self):
        for value in (0, 1e-8, 9e-6, 1e-5, 1.1e-5):
            self.assertAlmostEqual(self.transfer(.3, value, .3), value)

    def test_identity_and_zero_strength(self):
        for original in (0, .0001, .2, 1):
            self.assertEqual(self.transfer(original, .4, .4), original)
            self.assertEqual(self.transfer(original, .8, .4, 0), original)

    def test_matched_residual(self):
        self.assertAlmostEqual(self.transfer(.6, .3, .4), .5)
        self.assertAlmostEqual(self.transfer(.6, .3, .4, .5), .55)


class GuidePacking(unittest.TestCase):
    @staticmethod
    def texel(pixel, model_size, source_size, jitter_pixels, render_size, origin=0):
        uv = (pixel + .5) / model_size + jitter_pixels / render_size
        return origin + min(max(math.floor(uv * source_size), 0), source_size - 1)

    def test_source_contract(self):
        kernel = SHADER.split("void pack_guides(", 1)[1].split("#endif", 1)[0]
        self.assertIn("/ float2(g_sizes.zw) + g_jitter.xy", kernel)
        self.assertIn("floor(uv * float2(g_sizes.xy))", kernel)
        self.assertIn("floor(uv * float2(g_motion_size.xy))", kernel)
        self.assertIn("* g_motion_scale.xy + g_jitter.zw", kernel)
        self.assertIn("cb.jitter[0] - previous.jitter_x / static_cast<float>(previous.guide_w)", BRIDGE)
        self.assertIn("cb.jitter[1] - previous.jitter_y / static_cast<float>(previous.guide_h)", BRIDGE)
        self.assertIn("cb.motion_scale[0] = f.mv_scale_x / static_cast<float>(f.motion_w)", BRIDGE)
        self.assertIn('rect { "Depth", ww, wh }, rect { "MVec", ww, wh }', BRIDGE)
        self.assertIn("const float mv_x = static_cast<float>(ww)", BRIDGE)
        self.assertIn("const float mv_y = static_cast<float>(wh)", BRIDGE)
        self.assertIn("g_depth.Load(int3(id.xy, 0))", SHADER.split("void pack_mask(", 1)[1])

    def test_native_identity(self):
        for i in range(8):
            self.assertEqual(self.texel(i, 8, 8, 0, 8), i)

    def test_subrect_and_jitter_sign(self):
        self.assertEqual(self.texel(0, 2, 4, -.5, 4, 2), 2)
        self.assertEqual(self.texel(0, 2, 4, .5, 4, 2), 3)
        self.assertEqual(self.texel(0, 4, 4, .5, 4), 1)
        self.assertEqual(self.texel(0, 4, 4, -.5, 4), 0)

    def test_edges_stay_in_active_subrect(self):
        for model_size in (2, 4, 8):
            for jitter in (-.5, .5):
                for i in range(model_size):
                    at = self.texel(i, model_size, 4, jitter, 4, 3)
                    self.assertGreaterEqual(at, 3)
                    self.assertLess(at, 7)

    def test_independent_depth_and_motion_grids(self):
        self.assertEqual(self.texel(0, 2, 4, .5, 4, 2), 3)
        self.assertEqual(self.texel(0, 2, 8, .5, 4, 1), 4)

    def test_motion_units_across_model_sizes(self):
        for source_size, raw_pixels in ((4, 2), (8, 4)):
            uv = raw_pixels / source_size
            for model_size in (2, 4, 8):
                self.assertEqual(uv * model_size, model_size / 2)

    def test_remove_jitter_from_stationary_and_moving_vectors(self):
        current, previous, render_size = .25, -.25, 4
        correction = (current - previous) / render_size
        for true_motion in (0, -.1, .1):
            stored_pixels = true_motion * render_size + previous - current
            self.assertAlmostEqual(stored_pixels / render_size + correction, true_motion)


if __name__ == "__main__":
    unittest.main()
