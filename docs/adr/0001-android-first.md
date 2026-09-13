# 0001. Android first

Status: accepted. Date: 2026-09-03.

## Context

A throwaway iOS spike (MetalSplatter, CoreMotion, collider walking) proved the product on an iPhone in two days.
Research on Android found no embeddable Gaussian splat renderer, no `.spz` reader wired to a renderer, and no GPU sort proven on Adreno or Mali.
Android is where the open problem is.

## Decision

Build the Android engine first and treat it as library foundations, not a spike.
iOS is parked until the Android engine renders and walks; it then consumes the same shared core.

## Consequences

The first months produce no React Native surface.
Domains, tests and CI exist from the first commit so contributors can join per domain.
