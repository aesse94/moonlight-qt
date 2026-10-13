# Pending patches for the submodule and host forks

These belong on `frogtale-starlink` in [aesse94/moonlight-common-c](https://github.com/aesse94/moonlight-common-c)
and [aesse94/Apollo](https://github.com/aesse94/Apollo). They are kept here until they can be pushed there.

- `moonlight-common-c-delay-reports.patch`: the client reports queueing delay as `FTDL` every 250 ms.
- `apollo-delay-step-down.patch`: Apollo steps the bitrate down on those reports (`frogtale_delay_ms`).

Apply from each repo's `frogtale-starlink` checkout with `git am <patch>`. Then bump the
`moonlight-common-c` submodule here and remove this folder.
