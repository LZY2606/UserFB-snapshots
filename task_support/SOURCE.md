Upstream: https://github.com/mltframework/mlt
Commit: 2a51ed9cca0b7edca5f1c34749bad67b76b0051a

The original runtime files in src/framework/, COPYING are copied byte-for-byte. Optional plugins, device backends, language bindings and demos are excluded from this runtime export. The original copyright and license remain in the source files and license file.

task_support/ contains author-added build scaffolding and baseline observations, not an implementation of the requested changes. The direct static build uses only the system compiler, ar and platform threading/math libraries. Every current production translation unit under the selected runtime paths is compiled and archived, including files added later. No package manager or network access is used by these commands.
