#!/usr/bin/env perl
# scripts/sweep_mutants.pl - the mutant generator behind
# scripts/mutation_sweep.sh.
#
#   perl scripts/sweep_mutants.pl OUTDIR FILE...
#
# For every FILE (a path relative to the repository root, read from the
# current directory) it writes one mutated copy per mutant to
# OUTDIR/<key>/<FILE> and one manifest line per mutant to stdout:
#
#   key <TAB> file <TAB> line <TAB> operator <TAB> description
#
# Two operators, applied only inside function bodies (comments and
# string or character literals are masked out first):
#
#   swap    a binary relational operator written with whitespace on both
#           sides becomes its boundary or negated twin:
#           < to <=, <= to <, > to >=, >= to >, == to !=, != to ==
#   delete  one complete simple statement (from its first token to the ';'
#           that ends it, or a compound statement that fits on one line)
#           is removed
#
# The key is the first 10 hex digits of a SHA-1 over the file, the
# operator, the whitespace-normalised text the mutant changes (the line
# for a swap, the statement for a delete), the occurrence number of that
# text in the file and, for a swap, the operator's position on the line.
# Line numbers are not part of it, so a key survives edits elsewhere.
use strict;
use warnings;
use Digest::SHA qw(sha1_hex);
use File::Path qw(make_path);
use File::Basename qw(dirname);

my ($outdir, @files) = @ARGV;
die "usage: sweep_mutants.pl OUTDIR FILE...\n" unless defined $outdir && @files;

my %twin = ('<' => '<=', '<=' => '<', '>' => '>=', '>=' => '>',
            '==' => '!=', '!=' => '==');

for my $file (@files) {
    open my $fh, '<', $file or die "cannot read $file: $!\n";
    local $/;
    my $src = <$fh>;
    close $fh;
    $src =~ s/\r//g;
    my $masked = mask($src);
    my ($in_code, $stmts) = scan($masked);

    # Line starts, for offset -> line number.
    my @line_start = (0);
    while ($src =~ /\n/g) { push @line_start, pos($src); }
    my $line_of = sub {
        my ($off) = @_;
        my ($lo, $hi) = (0, $#line_start);
        while ($lo < $hi) {
            my $mid = int(($lo + $hi + 1) / 2);
            if ($line_start[$mid] <= $off) { $lo = $mid } else { $hi = $mid - 1 }
        }
        return $lo + 1;
    };
    my $line_text = sub {
        my ($n) = @_;
        my $s = $line_start[$n - 1];
        my $e = $n < @line_start ? $line_start[$n] - 1 : length($src);
        return substr($src, $s, $e - $s);
    };
    my $norm = sub { my $t = shift; $t =~ s/\s+/ /g; $t =~ s/^ | $//g; $t };

    my (%seen_line, %seen_stmt);

    # ---- relational swaps ------------------------------------------------
    my %line_ops;    # line -> list of [offset, op]
    while ($masked =~ /(?<=\s)(<=|>=|==|!=|<|>)(?=\s)/g) {
        my $op = $1;
        my $off = pos($masked) - length($op);
        next unless $in_code->[$off];
        push @{ $line_ops{ $line_of->($off) } }, [$off, $op];
    }
    my %line_occ;
    for my $n (1 .. scalar @line_start) {
        my $t = $norm->($line_text->($n));
        $line_occ{$n} = $seen_line{$t}++;
    }
    for my $n (sort { $a <=> $b } keys %line_ops) {
        my $text = $norm->($line_text->($n));
        my $k = 0;
        for my $o (@{ $line_ops{$n} }) {
            my ($off, $op) = @$o;
            my $to = $twin{$op};
            my $mut = $src;
            substr($mut, $off, length $op) = $to;
            my $key = substr(sha1_hex(join "\0", $file, 'swap', $text,
                                      $line_occ{$n}, $k, $op), 0, 10);
            my $mline = $line_text->($n);
            substr($mline, $off - $line_start[$n - 1], length $op) = $to;
            emit($outdir, $key, $file, $mut, $n, 'swap',
                 "$op -> $to: " . $norm->($mline));
            ++$k;
        }
    }

    # ---- statement deletions ---------------------------------------------
    for my $st (@$stmts) {
        my ($s, $e, $compound) = @$st;          # [s, e] inclusive
        my $l1 = $line_of->($s);
        my $l2 = $line_of->($e);
        next if $compound && $l1 != $l2;       # multi-line blocks are not
                                               # single statements
        my $text = $norm->(substr($src, $s, $e - $s + 1));
        my $occ = $seen_stmt{$text}++;
        my $mut = $src;
        my $ls = $line_start[$l1 - 1];
        my $le = $l2 < @line_start ? $line_start[$l2] : length($src);
        my $before = substr($src, $ls, $s - $ls);
        my $after = substr($src, $e + 1, $le - $e - 1);
        if ($before =~ /^\s*$/ && $after =~ /^\s*$/) {
            substr($mut, $ls, $le - $ls) = '';       # whole lines
        } else {
            substr($mut, $s, $e - $s + 1) = '';      # part of a line
        }
        my $key = substr(sha1_hex(join "\0", $file, 'delete', $text, $occ),
                         0, 10);
        emit($outdir, $key, $file, $mut, $l1, 'delete', $text);
    }
}

sub emit {
    my ($outdir, $key, $file, $mut, $line, $op, $desc) = @_;
    my $path = "$outdir/$key/$file";
    make_path(dirname($path));
    open my $oh, '>', $path or die "cannot write $path: $!\n";
    binmode $oh;
    print $oh $mut;
    close $oh;
    $desc =~ s/\t/ /g;
    print join("\t", $key, $file, $line, $op, $desc), "\n";
}

# Replace comments and string/character literals, keeping every offset
# and newline: comments become spaces, literals become underscores.
sub mask {
    my ($s) = @_;
    my $out = '';
    my $n = length $s;
    my $i = 0;
    while ($i < $n) {
        my $c = substr($s, $i, 1);
        my $two = substr($s, $i, 2);
        if ($two eq '//') {
            my $j = index($s, "\n", $i);
            $j = $n if $j < 0;
            $out .= ' ' x ($j - $i);
            $i = $j;
        } elsif ($two eq '/*') {
            my $j = index($s, '*/', $i + 2);
            $j = $j < 0 ? $n : $j + 2;
            (my $blank = substr($s, $i, $j - $i)) =~ s/[^\n]/ /g;
            $out .= $blank;
            $i = $j;
        } elsif ($c eq '"' || $c eq "'") {
            my $j = $i + 1;
            while ($j < $n) {
                my $d = substr($s, $j, 1);
                if ($d eq '\\') { $j += 2; next; }
                last if $d eq $c;
                ++$j;
            }
            $j = $n - 1 if $j >= $n;
            $out .= '_' x ($j - $i + 1);
            $i = $j + 1;
        } else {
            $out .= $c;
            ++$i;
        }
    }
    return $out;
}

# Walk the masked text with a stack of brace contexts:
#   file/scope  namespace, class or struct level (no statements)
#   block       a function body or a control block (statements live here)
#   init        a braced initialiser (part of the expression around it)
# Returns (\@in_code, \@statements): in_code[i] is true inside a function
# body; each statement is [start, end, compound].
sub scan {
    my ($t) = @_;
    my $n = length $t;
    my @in_code = (0) x $n;
    my @stmts;
    my @stack = ({ type => 'file', start => undef, paren => 0,
                   compound => 0 });
    my $prev_token = sub {       # previous significant token before $i
        my ($i) = @_;
        my $j = $i - 1;
        --$j while $j >= 0 && substr($t, $j, 1) =~ /\s/;
        return '' if $j < 0;
        my $c = substr($t, $j, 1);
        return $c unless $c =~ /\w/;
        my $k = $j;
        --$k while $k >= 0 && substr($t, $k, 1) =~ /\w/;
        return substr($t, $k + 1, $j - $k);
    };
    my $next_token = sub {       # next significant token after $i
        my ($i) = @_;
        my $j = $i + 1;
        ++$j while $j < $n && substr($t, $j, 1) =~ /\s/;
        return '' if $j >= $n;
        return $1 if substr($t, $j) =~ /^(\w+)/;
        return substr($t, $j, 1);
    };
    for (my $i = 0; $i < $n; ++$i) {
        my $c = substr($t, $i, 1);
        my $ctx = $stack[-1];
        my $code = grep { $_->{type} eq 'block' } @stack;
        $in_code[$i] = $code ? 1 : 0;
        next if $c =~ /\s/;
        if ($ctx->{type} eq 'block' && !defined $ctx->{start} && $c ne '}'
                && $c ne ';') {
            $ctx->{start} = $i;
            $ctx->{compound} = 0;
        }
        if ($c eq '(' || $c eq '[') { ++$ctx->{paren}; next; }
        if ($c eq ')' || $c eq ']') { --$ctx->{paren}; next; }
        if ($c eq '{') {
            my $p = $prev_token->($i);
            my $type;
            if ($ctx->{type} eq 'init') {
                $type = 'init';
            } elsif ($ctx->{type} eq 'block') {
                $type = ($p eq ')' || $p =~ /^(else|do|try|const|noexcept|mutable)$/
                         || $p eq ';' || $p eq '{' || $p eq '}' || $p eq '')
                        ? 'block' : 'init';
            } else {
                $type = ($p eq ')' || $p =~ /^(const|noexcept|override)$/)
                        ? 'block' : ($p eq '=' ? 'init' : 'scope');
            }
            $ctx->{compound} = 1 if $type eq 'block' && $ctx->{type} eq 'block';
            push @stack, { type => $type, start => undef, paren => 0,
                           compound => 0 };
            next;
        }
        if ($c eq '}') {
            my $closed = pop @stack;
            $ctx = $stack[-1];
            if ($closed->{type} eq 'block' && $ctx->{type} eq 'block'
                    && defined $ctx->{start} && $ctx->{paren} == 0) {
                my $nx = $next_token->($i);
                if ($nx !~ /^(else|;|\)|,|\()$/) {
                    push @stmts, [$ctx->{start}, $i, 1];
                    $ctx->{start} = undef;
                }
            }
            next;
        }
        if ($c eq ';' && $ctx->{type} eq 'block' && $ctx->{paren} == 0) {
            push @stmts, [$ctx->{start}, $i, $ctx->{compound}]
                if defined $ctx->{start};
            $ctx->{start} = undef;
            next;
        }
    }
    return (\@in_code, \@stmts);
}
