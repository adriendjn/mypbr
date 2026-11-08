"""mypbr's principled / metal BRDF as a Mitsuba 3 plugin ("mypbr_principled").

The same model as include/material.h, written again from the formulas rather
than ported line by line, so that rendering it with Mitsuba's integrator checks
mypbr's implementation (eval, pdf, sampling, MIS) against an independent one:

    f cos_i = F(h) D(h) G2(o, i) / (4 cos_o)              GGX, Schlick Fresnel,
            + F_ms f_ms(o, i) cos_i                       height-correlated Smith G
            + (1 - metallic) (1 - F_d(i)) (1 - F_d(o)) cos_i / pi * base_color

with f0 = lerp(((ior - 1) / (ior + 1))^2, base_color, metallic), F_d the
Schlick Fresnel of the dielectric f0, and the Kulla-Conty lobe

    f_ms = (1 - E(mu_o)) (1 - E(mu_i)) / (pi (1 - E_avg)),
    F_ms = F_avg^2 E_avg / (1 - F_avg (1 - E_avg)),  F_avg = f0 + (1 - f0) / 21.

mypbr's metal (roughness > 0) is this with metallic = 1.

With a clear coat of weight w and its own roughness (alpha_c), the above
becomes f_base and

    f cos_i = w F_c(o.h) D_c G2_c / (4 cos_o) + w F_ms,c f_ms,c cos_i
            + (1 - w F_c(o)) (1 - w F_c(i)) f_base cos_i

with F_c the Schlick Fresnel of f0 = 0.04 (ior 1.5), on the same normal.

E(mu) is computed here by quadrature over the GGX distribution of normals
(mypbr's ggx_albedo_table uses stratified VNDF sampling instead), for the
material's exact alpha rather than interpolated from a 32 x 32 table.

Sampling does not need to match mypbr's, only to be correct: a fixed mixture
of visible-normal GGX reflection and a cosine lobe.

Import this module after mi.set_variant(); it registers the plugin.
"""

import drjit as dr
import mitsuba as mi
import numpy as np


def roughness_to_alpha(r):
    return max(r * r, 1e-3)


def ggx_lambda_np(cos_theta, alpha):
    cos2 = np.maximum(cos_theta * cos_theta, 1e-12)
    tan2 = np.maximum(1 - cos2, 0) / cos2
    return (-1 + np.sqrt(1 + alpha * alpha * tan2)) / 2


def ggx_albedo(alpha, mus, n_u=1024, n_phi=512):
    """E(mu) = integral of D G2 / (4 mu) d(omega_i) with F = 1, for each mu.

    Substituting the half vector h (d omega_i = 4 (o.h) d omega_h) and then
    u = tan^2(theta_h) / (alpha^2 + tan^2(theta_h)), for which D cos(theta_h)
    d omega_h = du dphi / (2 pi), turns it into the average over (u, phi) of
    G2 (o.h) / (mu cos(theta_h)), counted where o.h > 0 and i is above."""
    u = (np.arange(n_u) + 0.5) / n_u
    phi = (np.arange(n_phi) + 0.5) / n_phi * 2 * np.pi
    U, PHI = np.meshgrid(u, phi, indexing="ij")
    tan2 = alpha * alpha * U / (1 - U)
    cos_h = 1 / np.sqrt(1 + tan2)
    sin_h = np.sqrt(1 - cos_h * cos_h)
    h = np.stack([sin_h * np.cos(PHI), sin_h * np.sin(PHI), cos_h], axis=-1)
    result = []
    for mu in mus:
        mu = max(mu, 1e-3)
        o = np.array([np.sqrt(1 - mu * mu), 0, mu])
        oh = h @ o
        i = 2 * oh[..., None] * h - o
        valid = (oh > 0) & (i[..., 2] > 0)
        g2 = 1 / (1 + ggx_lambda_np(mu, alpha) + ggx_lambda_np(i[..., 2], alpha))
        integrand = np.where(valid, g2 * oh / (mu * cos_h), 0)
        result.append(min(integrand.mean(), 1.0))
    return np.array(result)


class GGXLobe:
    """A GGX lobe of one alpha: its distribution, Smith Lambda, the E(mu)
    table and the Kulla-Conty term."""
    TABLE = 129  # E(mu) samples, mu = 0 .. 1

    def __init__(self, alpha):
        self.alpha = alpha
        mus = np.linspace(0, 1, self.TABLE)
        e = ggx_albedo(alpha, mus)
        # E_avg = 2 * integral of E(mu) mu dmu (trapezoids)
        self.e_avg = float(np.trapezoid(2 * e * mus, mus))
        self.e_table = mi.Float(e)

    def distr(self):
        return mi.MicrofacetDistribution(mi.MicrofacetType.GGX, self.alpha, True)

    def lam(self, cos_theta):
        cos2 = dr.maximum(dr.square(cos_theta), 1e-12)
        tan2 = dr.maximum(1 - cos2, 0) / cos2
        return (-1 + dr.sqrt(1 + self.alpha * self.alpha * tan2)) / 2

    def E(self, mu):
        x = dr.clip(mu, 0, 1) * (self.TABLE - 1)
        i = dr.minimum(mi.UInt32(dr.floor(x)), self.TABLE - 2)
        t = x - mi.Float(i)
        return dr.lerp(dr.gather(mi.Float, self.e_table, i), dr.gather(mi.Float, self.e_table, i + 1), t)

    def single(self, fresnel, cos_v, cos_l, h):
        """F D G2 / (4 cos_v), for the Fresnel value already computed."""
        G2 = 1 / (1 + self.lam(cos_v) + self.lam(cos_l))
        return fresnel * (self.distr().eval(h) * G2 / (4 * cos_v))

    def multiple(self, f0, cos_v, cos_l):
        """F_ms f_ms cos_l, the Kulla-Conty lobe (0 when nothing is missing)."""
        if 1 - self.e_avg <= 1e-6:
            return 0
        f_avg = f0 + (1 - f0) / 21
        f_ms = dr.square(f_avg) * self.e_avg / (1 - f_avg * (1 - self.e_avg))
        return f_ms * ((1 - self.E(cos_v)) * (1 - self.E(cos_l)) / (dr.pi * (1 - self.e_avg)) * cos_l)

    def pdf(self, wi, wo, h):
        return self.distr().pdf(wi, h) / (4 * dr.abs(dr.dot(wo, h)))


class MypbrPrincipled(mi.BSDF):
    def __init__(self, props):
        mi.BSDF.__init__(self, props)
        base = props["base_color"]
        # An <rgb> value can arrive as a texture or as a plain color
        self.base_color = base if hasattr(base, "eval") else mi.Color3f(base[0], base[1], base[2])
        self.metallic = float(props.get("metallic", 0.0))
        self.roughness = min(max(float(props.get("roughness", 0.5)), 0.0), 1.0)
        ior = float(props.get("ior", 1.5))
        self.alpha = roughness_to_alpha(self.roughness)
        self.f0_dielectric = ((ior - 1) / (ior + 1)) ** 2
        self.p_spec = 0.5 + 0.4 * self.metallic
        # false drops the Kulla-Conty lobe: plain single-scattering GGX
        self.compensation = bool(props.get("compensation", True))
        self.lobe = GGXLobe(self.alpha)

        # Clear coat: weight 0 means none
        self.clearcoat = min(max(float(props.get("clearcoat", 0.0)), 0.0), 1.0)
        coat_roughness = min(max(float(props.get("clearcoat_roughness", 0.0)), 0.0), 1.0)
        self.coat = GGXLobe(roughness_to_alpha(coat_roughness)) if self.clearcoat > 0 else None
        self.p_coat = 0.3 if self.coat else 0.0

        flags = mi.BSDFFlags.GlossyReflection | mi.BSDFFlags.DiffuseReflection | mi.BSDFFlags.FrontSide
        self.m_components = [flags]
        self.m_flags = flags

    # --- Model ---

    def schlick(self, cos_theta, f0):
        m = dr.clip(1 - cos_theta, 0, 1)
        return f0 + (1 - f0) * dr.square(dr.square(m)) * m

    def f0(self, base):
        return dr.lerp(mi.Color3f(self.f0_dielectric), base, self.metallic)

    def eval_pdf(self, ctx, si, wo, active=True):
        """f cos and pdf; wi (si.wi) is towards the viewer, wo towards the light."""
        cos_v = mi.Frame3f.cos_theta(si.wi)
        cos_l = mi.Frame3f.cos_theta(wo)
        active = active & (cos_v > 0) & (cos_l > 0)

        base = (mi.Color3f(self.base_color.eval(si, active)) if hasattr(self.base_color, "eval")
                else self.base_color)
        f0 = self.f0(base)
        h = dr.normalize(si.wi + wo)
        spec = self.lobe.single(self.schlick(dr.dot(wo, h), f0), cos_v, cos_l, h)
        if self.compensation:
            spec += self.lobe.multiple(f0, cos_v, cos_l)

        transmitted = (1 - self.schlick(cos_l, self.f0_dielectric)) * (1 - self.schlick(cos_v, self.f0_dielectric))
        diffuse = base * ((1 - self.metallic) * transmitted * cos_l / dr.pi)
        value = spec + diffuse
        pdf = self.p_spec * self.lobe.pdf(si.wi, wo, h) + (1 - self.p_spec) * cos_l / dr.pi

        if self.coat:
            w = self.clearcoat
            through = (1 - w * self.schlick(cos_v, 0.04)) * (1 - w * self.schlick(cos_l, 0.04))
            coat = self.coat.single(self.schlick(dr.dot(wo, h), 0.04), cos_v, cos_l, h)
            if self.compensation:
                coat += self.coat.multiple(0.04, cos_v, cos_l)
            value = through * value + w * coat
            pdf = self.p_coat * self.coat.pdf(si.wi, wo, h) + (1 - self.p_coat) * pdf

        value = dr.select(active, value, 0)
        pdf = dr.select(active, pdf, 0)
        return value, pdf

    # --- Mitsuba interface ---

    def sample(self, ctx, si, sample1, sample2, active=True):
        cos_v = mi.Frame3f.cos_theta(si.wi)
        active = active & (cos_v > 0)
        # sample1 picks the lobe: coat, then base specular, then cosine
        m, _ = self.lobe.distr().sample(si.wi, sample2)
        u = (sample1 - self.p_coat) / (1 - self.p_coat)
        wo = dr.select(u < self.p_spec, mi.reflect(si.wi, m),
                       mi.warp.square_to_cosine_hemisphere(sample2))
        if self.coat:
            m_coat, _ = self.coat.distr().sample(si.wi, sample2)
            wo = dr.select(sample1 < self.p_coat, mi.reflect(si.wi, m_coat), wo)
        value, pdf = self.eval_pdf(ctx, si, wo, active)
        active = active & (pdf > 0) & (mi.Frame3f.cos_theta(wo) > 0)

        bs = mi.BSDFSample3f()
        bs.wo = wo
        bs.pdf = pdf
        bs.eta = 1.0
        bs.sampled_type = mi.UInt32(+mi.BSDFFlags.GlossyReflection)
        bs.sampled_component = mi.UInt32(0)
        return bs, dr.select(active, value / dr.maximum(pdf, 1e-20), 0)

    def eval(self, ctx, si, wo, active=True):
        return self.eval_pdf(ctx, si, wo, active)[0]

    def pdf(self, ctx, si, wo, active=True):
        return self.eval_pdf(ctx, si, wo, active)[1]

    def traverse(self, callback):
        pass

    def to_string(self):
        return (f"MypbrPrincipled[metallic={self.metallic}, roughness={self.roughness}, "
                f"alpha={self.alpha}, E_avg={self.lobe.e_avg:.4f}, clearcoat={self.clearcoat}]")


mi.register_bsdf("mypbr_principled", lambda props: MypbrPrincipled(props))
