/*
 * Stoatworks Labs - About window data for Handleman.
 *
 * PROVISIONAL HAND COPY, adapted from gate's generated file on 2026-10-16:
 * Handleman is not registered in the website's projects.json yet, so
 * stoatworks-backend/scripts/sync-about.py has nothing to generate it from.
 * There is no user guide, so `guide` is empty. The first sync after
 * registration overwrites this file.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Handleman";
    inline constexpr auto slug = "handleman";
    inline constexpr auto hook = "A hand-cranked camera and projector, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/handleman/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/handleman";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
