% Generate non-square complex double with -v7.3 gzip+multi-chunk for matio tests.
% 200x32 fails on stock matio 1.6.0 (chunks typically (32,100)); square /
% single-chunk / many "nice" sizes (e.g. 256x32, 200x100) do not catch the bug.
%
% Run (ASCII path recommended for older MATLAB):
%   matlab -nosplash -nodesktop -r "make_nonsquare_complex_gzip; exit"
% Then regenerate golden with a fixed build:
%   test_mat readvar nonsquare_complex_gzip_le.mat var_nonsquare > ../results/read-var_nonsquare.out

here = fileparts(mfilename('fullpath'));
nr = 200;
nc = 32;
n = nr * nc;
var_nonsquare = reshape(complex(1:n, (n + 1):(2 * n)), nr, nc);
out = fullfile(here, 'nonsquare_complex_gzip_le.mat');
save(out, 'var_nonsquare', '-v7.3');
fprintf('Wrote %s size=[%d %d]\n', out, nr, nc);
