###################################################
# Samba4 NDR info tree generator
# Copyright tridge@samba.org 2000-2003
# Copyright tpot@samba.org 2001
# Copyright jelmer@samba.org 2004-2006
# released under the GNU GPL

=pod

=head1 NAME

Parse::Pidl::NDR - NDR parsing information generator

=head1 DESCRIPTION

Return a table describing the order in which the parts of an element
should be parsed
Possible level types:
 - POINTER
 - ARRAY
 - SUBCONTEXT
 - SWITCH
 - DATA

=head1 AUTHOR

Jelmer Vernooij <jelmer@samba.org>

=cut

package Parse::Pidl::NDR;

require Exporter;
use vars qw($VERSION);
$VERSION = '0.01';
@ISA = qw(Exporter);
@EXPORT = qw(GetPrevLevel GetNextLevel ContainsDeferred ContainsString);

use strict;
use Parse::Pidl::Typelist qw(hasType getType expandAlias);
use Parse::Pidl::Util qw(has_property property_matches);

# Alignment of the built-in scalar types
my $scalar_alignment = {
	'void' => 0,
	'char' => 1,
	'int8' => 1,
	'uint8' => 1,
	'int16' => 2,
	'uint16' => 2,
	'int32' => 4,
	'uint32' => 4,
	'hyper' => 8,
	'pointer' => 8,
	'dlong' => 4,
	'udlong' => 4,
	'udlongr' => 4,
	'DATA_BLOB' => 4,
	'string' => 4,
	'string_array' => 4, #???
	'time_t' => 4,
	'NTTIME' => 4,
	'NTTIME_1sec' => 4,
	'NTTIME_hyper' => 8,
	'WERROR' => 4,
	'NTSTATUS' => 4,
	'COMRESULT' => 4,
	'nbt_string' => 4,
	'wrepl_nbt_name' => 4,
	'ipv4address' => 4
};

sub nonfatal($$)
{
	my ($e,$s) = @_;
	warn ("$e->{FILE}:$e->{LINE}: warning: $s\n");
}

#####################################################################
# signal a fatal validation error
sub fatal($$)
{
	my ($pos,$s) = @_;
	die("$pos->{FILE}:$pos->{LINE}:$s\n");
}

sub GetElementLevelTable($)
{
	my $e = shift;

	my $order = [];
	my $is_deferred = 0;
	my @bracket_array = ();
	my @length_is = ();
	my @size_is = ();
	my $pointer_idx = 0;
	my $pointers = $e->{POINTERS};
	my $has_string = has_property($e, "string");
	my $dt = $e->{TYPEREF};
#warn "GetElementLevelTable($e->{NAME}):$e->{LINE} " . (caller(0))[3] . (caller(0))[2];
	$pointers += $dt->{DATA}{POINTERS} if ($dt->{DATA}{POINTERS});

	if (has_property($e, "size_is")) {
		@size_is = split /,/, has_property($e, "size_is");
	}

	if (has_property($e, "length_is")) {
		@length_is = split /,/, has_property($e, "length_is");
	}

	if (defined($e->{ARRAY_LEN})) {
		@bracket_array = @{$e->{ARRAY_LEN}};
	}

	if (has_property($e, "out")) {
		my $needptrs = 1;

		if (has_property($e, "string")) { $needptrs++; }
		if ($#bracket_array >= 0) { $needptrs = 0; }

		nonfatal($e, "[out] argument `$e->{NAME}' not a pointer") if ($needptrs > $e->{POINTERS});
	}

	# Parse the [][][][] style array stuff
	for my $i (0 .. $#bracket_array) {
		my $d = $bracket_array[$#bracket_array - $i];
		my $size = $d;
		my $length = $d;
		my $is_surrounding = 0;
		my $is_varying = 0;
		my $is_conformant = 0;
		my $is_string = 0;

		if ($d eq "*") {
			$is_conformant = 1;
			if ($size = shift @size_is) {
			} elsif ((scalar(@size_is) == 0) and has_property($e, "string")) {
				$is_string = 1;
				delete($e->{PROPERTIES}->{string});
			} else {
				fatal($e, "Must specify size_is() for conformant array!")
			}

			if (($length = shift @length_is) or $is_string) {
				$is_varying = 1;
			} else {
				$length = $size;
			}

			if ($e == $e->{PARENT}->{ELEMENTS}[-1] 
				and $e->{PARENT}->{TYPE} ne "FUNCTION") {
				$is_surrounding = 1;
			}
		}

		push (@$order, {
			TYPE => "ARRAY",
			SIZE_IS => $size,
			LENGTH_IS => $length,
			IS_DEFERRED => "$is_deferred",
			IS_SURROUNDING => "$is_surrounding",
			IS_ZERO_TERMINATED => "$is_string",
			IS_VARYING => "$is_varying",
			IS_CONFORMANT => "$is_conformant",
			IS_FIXED => (not $is_conformant and Parse::Pidl::Util::is_constant($size)),
			IS_INLINE => (not $is_conformant and not Parse::Pidl::Util::is_constant($size))
		});
	}

	# Next, all the pointers
	foreach my $i (1..$pointers) {
		my $pt = pointer_type($e);

		my $level = "EMBEDDED";
		# Top level "ref" pointers do not have a referrent identifier
		$level = "TOP" if ( defined($pt) 
				and $i == 1
				and $e->{PARENT}->{TYPE} eq "FUNCTION");

		push (@$order, { 
			TYPE => "POINTER",
			# FIXME:avg POINTER_TYPE better resolution (use defaultpointer???)
			POINTER_TYPE => ($e->{PARENT} and $e->{PARENT}->{TYPE} eq "FUNCTION") ? (($level eq "TOP") ? $pt : "unique" ) : $pt,
			POINTER_INDEX => $pointer_idx,
			IS_DEFERRED => "$is_deferred",
			DATA_TYPE => ($i > $e->{POINTERS}) ? $e->{TYPE} : undef,
			LEVEL => $level
		});

		nonfatal($e, "top-level \[out\] pointer `$e->{NAME}' is not a \[ref\] pointer") 
			if ($i == 1 and $e->{PARENT} and
				$e->{PARENT}->{TYPE} eq "FUNCTION" and
				pointer_type($e) ne "ref" and
				not has_property($e, "in"));

		$pointer_idx++;
		
		# everything that follows will be deferred
		$is_deferred = 1 if ($e->{PARENT} and $e->{PARENT}->{TYPE} ne "FUNCTION");

		my $array_size = shift @size_is;
		my $array_length;
		my $is_varying;
		my $is_conformant;
		my $is_string = 0;
		if ($array_size) {
			$is_conformant = 1;
			if ($array_length = shift @length_is) {
				$is_varying = 1;
			} else {
				$array_length = $array_size;
				$is_varying =0;
			}
		} 
		
		if (scalar(@size_is) == 0 and has_property($e, "string") and 
		    $i == $pointers) {
			$is_string = 1;
			$is_varying = $is_conformant = has_property($e, "noheader")?0:1;
			delete($e->{PROPERTIES}->{string});
		}

		if ($array_size or $is_string) {
			push (@$order, {
				TYPE => "ARRAY",
				IS_ZERO_TERMINATED => "$is_string",
				SIZE_IS => $array_size,
				LENGTH_IS => $array_length,
				IS_DEFERRED => "$is_deferred",
				IS_SURROUNDING => 0,
				IS_VARYING => "$is_varying",
				IS_CONFORMANT => "$is_conformant",
				IS_FIXED => 0,
				DATA_TYPE => ($i > $e->{POINTERS}) ? $e->{TYPE} : undef,
				IS_INLINE => 0,
			});

			$is_deferred = 0;
		} 
	}

	if (defined(has_property($e, "subcontext"))) {
		my $hdr_size = has_property($e, "subcontext");
		my $subsize = has_property($e, "subcontext_size");
		if (not defined($subsize)) { 
			$subsize = -1; 
		}
		
		push (@$order, {
			TYPE => "SUBCONTEXT",
			HEADER_SIZE => $hdr_size,
			SUBCONTEXT_SIZE => $subsize,
			IS_DEFERRED => $is_deferred,
			COMPRESSION => has_property($e, "compression"),
		});
	}

	if (my $switch = has_property($e, "switch_is")) {
		push (@$order, {
			TYPE => "SWITCH", 
			SWITCH_IS => $switch,
			IS_DEFERRED => $is_deferred
		});
	}

	if (scalar(@size_is) > 0) {
		nonfatal($e, "size_is() on non-array element");
	}

	if (scalar(@length_is) > 0) {
		nonfatal($e, "length_is() on non-array element");
	}

	if (has_property($e, "string")) {
		nonfatal($e, "string() attribute on non-array element");
	}
	
	push (@$order, {
		TYPE => "DATA",
		CONVERT_TO => has_property($e, ""),
		CONVERT_FROM => has_property($e, ""),
		DATA_TYPE => defined($dt->{DATA}{DATA_TYPE}) ? $dt->{DATA}{DATA_TYPE} : $e->{TYPE},
		IS_DEFERRED => $is_deferred,
		CONTAINS_DEFERRED => can_contain_deferred($e),
		IS_SURROUNDING => 0 #FIXME
	});

	my $i = 0;
	foreach (@$order) { $_->{LEVEL_INDEX} = $i; $i+=1; }

	$e->{PROPERTIES}->{string} = 1 if ($has_string);
	return $order;
}

#####################################################################
# see if a type contains any deferred data 
sub can_contain_deferred
{
	my $e = shift;

	return 0 if (Parse::Pidl::Typelist::is_scalar($e->{TYPE}));
	return 1 unless (hasType($e->{TYPE})); # assume the worst

	my $type = getType($e->{TYPE});

	foreach my $x (@{$type->{DATA}->{ELEMENTS}}) {
		return 1 if ($x->{POINTERS});
#		my $dt = getType($x->{TYPE});
		return 1 if ($x->{TYPEREF}{DATA}{POINTERS});
		return 1 if (can_contain_deferred ($x));
	}
	
	return 0;
}

sub pointer_type($)
{
	my $e = shift;

#	return undef unless $e->{POINTERS};
	
	return "ref" if (has_property($e, "ref"));
	return "full" if (has_property($e, "ptr"));
	return "sptr" if (has_property($e, "sptr"));
	return "unique" if (has_property($e, "unique"));
	return "relative" if (has_property($e, "relative"));
	return "ignore" if (has_property($e, "ignore"));

	return undef;
}

#####################################################################
# work out the correct alignment for a structure or union
sub find_largest_alignment($)
{
	my $s = shift;

	my $align = 1;
	for my $e (@{$s->{ELEMENTS}}) {
		my $a = 1;

		if ($e->{POINTERS}) {
			$a = 4; 
		} elsif (has_property($e, "subcontext")) { 
			$a = 1;
		} elsif (has_property($e, "transmit_as")) {
			$a = align_type($e->{PROPERTIES}->{transmit_as});
		} else {
			$a = align_type($e->{TYPE}); 
		}

		$align = $a if ($align < $a);
	}

	return $align;
}

#####################################################################
# align a type
sub align_type($)
{
	sub align_type($);
	my $e = shift;

	unless (hasType($e)) {
	    # it must be an external type - all we can do is guess 
		# print "Warning: assuming alignment of unknown type '$e' is 4\n";
	    return 4;
	}

	my $dt = getType($e)->{DATA};

	if ($dt->{TYPE} eq "ENUM") {
		return align_type(Parse::Pidl::Typelist::enum_type_fn($dt));
	} elsif ($dt->{TYPE} eq "BITMAP") {
		return align_type(Parse::Pidl::Typelist::bitmap_type_fn($dt));
	} elsif (($dt->{TYPE} eq "STRUCT") or ($dt->{TYPE} eq "UNION")) {
		return find_largest_alignment($dt);
	} elsif ($dt->{TYPE} eq "SCALAR") {
		return $scalar_alignment->{$dt->{NAME}};
	} elsif ($dt->{TYPE} eq "DECORATED") {
		return $scalar_alignment->{$dt->{DATA_TYPE}};
	} elsif ($dt->{TYPE} eq "INTERFACE") {
		return 4;
	}

	die("Unknown data type type $dt->{TYPE}");
}

sub ParseElement($)
{
	my $e = shift;

	$e->{TYPE} = expandAlias($e->{TYPE});
	$e->{TYPEREF} = getType($e->{TYPE});

	my $dt = $e->{TYPEREF};
	if ($dt) {
		for my $s ("string","charset") {
			$e->{PROPERTIES}{$s} = $dt->{PROPERTIES}{$s} if (!defined($e->{PROPERTIES}{$s}) and $dt->{PROPERTIES}{$s});
		}
	}
	
	return {
		NAME => $e->{NAME},
		TYPE => $e->{TYPE},
		TYPEREF => $dt,
		PROPERTIES => $e->{PROPERTIES},
		LEVELS => GetElementLevelTable($e),
		REPRESENTATION_TYPE => $e->{PROPERTIES}->{represent_as},
		ALIGN => align_type($e->{TYPE}),
		ORIGINAL => $e
	};
}

sub ParseStruct($$)
{
	my ($ndr,$struct) = @_;
	my @elements = ();
	my $surrounding = undef;

	foreach my $x (@{$struct->{ELEMENTS}}) 
	{
		my $e = ParseElement($x);
		if ($x != $struct->{ELEMENTS}[-1] and 
			$e->{LEVELS}[0]->{IS_SURROUNDING}) {
			print "$x->{FILE}:$x->{LINE}: error: conformant member not at end of struct\n";
		}
		push @elements, $e;
	}

	my $e = $elements[-1];
	if (defined($e) and defined($e->{LEVELS}[0]->{IS_SURROUNDING}) and
		$e->{LEVELS}[0]->{IS_SURROUNDING}) {
		$surrounding = $e;
	}

	if (defined $e->{TYPE} && $e->{TYPE} eq "string"
	    &&  property_matches($e, "flag", ".*LIBNDR_FLAG_STR_CONFORMANT.*")) {
		$surrounding = $struct->{ELEMENTS}[-1];
	}
		
	return {
		TYPE => "STRUCT",
		SURROUNDING_ELEMENT => $surrounding,
		ELEMENTS => \@elements,
		PROPERTIES => $struct->{PROPERTIES},
		ORIGINAL => $struct
	};
}

sub ParseUnion($$)
{
	my ($ndr,$e) = @_;
	my @elements = ();
	my $switch_type = has_property($e, "switch_type");
	unless (defined($switch_type)) { $switch_type = "uint32"; }

	if (has_property($e, "nodiscriminant")) { $switch_type = undef; }
	
	my $hasdefault = 0;
	foreach my $x (@{$e->{ELEMENTS}}) 
	{
		my $t;
		if ($x->{TYPE} eq "EMPTY") {
			$t = { TYPE => "EMPTY" };
		} else {
			$t = ParseElement($x);
		}
		if (has_property($x, "default")) {
			$t->{CASE} = "default";
			$hasdefault = 1;
		} elsif (defined($x->{PROPERTIES}->{case})) {
			$t->{CASE} = "case $x->{PROPERTIES}->{case}";
		} else {
			die("Union element $x->{NAME} has neither default nor case property");
		}
		push @elements, $t;
	}

	return {
		TYPE => "UNION",
		SWITCH_TYPE => $switch_type,
		ELEMENTS => \@elements,
		PROPERTIES => $e->{PROPERTIES},
		HAS_DEFAULT => $hasdefault,
		ORIGINAL => $e
	};
}

sub ParseEnum($$)
{
	my ($ndr,$e) = @_;

	return {
		TYPE => "ENUM",
		BASE_TYPE => Parse::Pidl::Typelist::enum_type_fn($e),
		ELEMENTS => $e->{ELEMENTS},
		PROPERTIES => $e->{PROPERTIES},
		ORIGINAL => $e
	};
}

sub ParseBitmap($$)
{
	my ($ndr,$e) = @_;

	return {
		TYPE => "BITMAP",
		BASE_TYPE => Parse::Pidl::Typelist::bitmap_type_fn($e),
		ELEMENTS => $e->{ELEMENTS},
		PROPERTIES => $e->{PROPERTIES},
		ORIGINAL => $e
	};
}

sub ParseDecorated($$)
{
	my ($ndr,$e) = @_;

	return {
		TYPE => "DECORATED",
		NAME => $e->{NAME},
		PROPERTIES => $e->{PROPERTIES},
#		LEVELS => GetElementLevelTable($e->{DATA}),
		ORIGINAL => $e
	};
}

sub ParseType($$)
{
	my ($ndr, $d) = @_;

	if ($d->{TYPE} eq "STRUCT" or $d->{TYPE} eq "UNION") {
		CheckPointerTypes($d, $ndr->{PROPERTIES}->{pointer_default});
	}

	my $data = {
		STRUCT => \&ParseStruct,
		UNION => \&ParseUnion,
		ENUM => \&ParseEnum,
		BITMAP => \&ParseBitmap,
		TYPEDEF => \&ParseTypedef,
		DECORATED => \&ParseDecorated,
	}->{$d->{TYPE}}->($ndr, $d);

	return $data;
}

sub ParseTypedef($$)
{
	my ($ndr,$d) = @_;

	if (defined($d->{PROPERTIES}) && !defined($d->{DATA}->{PROPERTIES})) {
		$d->{DATA}->{PROPERTIES} = $d->{PROPERTIES};
	}

	my $data = ParseType($ndr, $d->{DATA});
	$data->{ALIGN} = align_type($d->{NAME});

	return {
		NAME => $d->{NAME},
		TYPE => $d->{TYPE},
		PROPERTIES => $d->{PROPERTIES},
		DATA => $data,
		ORIGINAL => $d
	};
}

sub ParseConst($$)
{
	my ($ndr,$d) = @_;

	return $d;
}

sub ParseFunction($$$)
{
	my ($ndr,$d,$opnum) = @_;
	my @elements = ();
	my $rettype = undef;
	my $thisopnum = undef;

	CheckPointerTypes($d, $ndr->{PROPERTIES}->{pointer_default_top});

	if (not defined($d->{PROPERTIES}{noopnum})) {
		$thisopnum = ${$opnum};
		${$opnum}++;
	}

	foreach my $x (@{$d->{ELEMENTS}}) {
		my $e = ParseElement($x);
		push (@{$e->{DIRECTION}}, "in") if (has_property($x, "in"));
		push (@{$e->{DIRECTION}}, "out") if (has_property($x, "out"));

		push (@elements, $e);
	}

	if ($d->{RETURN_TYPE} ne "void") {
		$rettype = expandAlias($d->{RETURN_TYPE});
	}
	
	my $async = 0;
	if (has_property($d, "async")) { $async = 1; }
	
	return {
			NAME => $d->{NAME},
			TYPE => "FUNCTION",
			OPNUM => $thisopnum,
			ASYNC => $async,
			RETURN_TYPE => $rettype,
			PROPERTIES => $d->{PROPERTIES},
			ELEMENTS => \@elements,
			ORIGINAL => $d
		};
}

sub CheckPointerTypes($$)
{
	my ($s,$default) = @_;

	foreach my $e (@{$s->{ELEMENTS}}) {
		my $is_ptr = $e->{POINTERS};
		my $dt = getType($e->{TYPE});
		$is_ptr += $dt->{DATA}{POINTERS} if defined($dt->{DATA}{POINTERS});
		if ($is_ptr and not defined(pointer_type($e))) {
			$e->{PROPERTIES}->{$default} = 1;
		}
	}
}

sub ParseInterface($)
{
	my $idl = shift;
	my @types = ();
	my @consts = ();
	my @functions = ();
	my @endpoints;
	my @declares = ();
	my $opnum = 0;
	my $version;

	if (not has_property($idl, "pointer_default")) {
		# MIDL defaults to "ptr" in DCE compatible mode (/osf)
		# and "unique" in Microsoft Extensions mode (default)
		$idl->{PROPERTIES}->{pointer_default} = "unique";
	}

	if (not has_property($idl, "pointer_default_top")) {
		$idl->{PROPERTIES}->{pointer_default_top} = "ref";
	} else {
		nonfatal($idl, "pointer_default_top() is a pidl extension and should not be used");
	}

	foreach my $d (@{$idl->{DATA}}) {
		if ($d->{TYPE} eq "DECLARE") {
			push (@declares, $d);
		} elsif ($d->{TYPE} eq "FUNCTION") {
			push (@functions, ParseFunction($idl, $d, \$opnum));
		} elsif ($d->{TYPE} eq "CONST") {
			push (@consts, ParseConst($idl, $d));
		} else {
			push (@types, ParseType($idl, $d));
		}
	}

	$version = "0.0";

	if(defined $idl->{PROPERTIES}->{version}) { 
		$version = $idl->{PROPERTIES}->{version}; 
	}

	# If no endpoint is set, default to the interface name as a named pipe
	if (!defined $idl->{PROPERTIES}->{endpoint}) {
		push @endpoints, "\"ncacn_np:[\\\\pipe\\\\" . $idl->{NAME} . "]\"";
	} else {
		@endpoints = split /,/, $idl->{PROPERTIES}->{endpoint};
	}

	return { 
		NAME => $idl->{NAME},
		UUID => lc(has_property($idl, "uuid")),
		VERSION => $version,
		TYPE => "INTERFACE",
		PROPERTIES => $idl->{PROPERTIES},
		FUNCTIONS => \@functions,
		CONSTS => \@consts,
		TYPES => \@types,
		DECLARES => \@declares,
		ENDPOINTS => \@endpoints
	};
}

# Convert a IDL tree to a NDR tree
# Gives a result tree describing all that's necessary for easily generating
# NDR parsers / generators
sub Parse($)
{
	my $idl = shift;

	return undef unless (defined($idl));

	Parse::Pidl::NDR::Validate($idl);
	
	my @ndr = ();

	foreach (@{$idl}) {
		($_->{TYPE} eq "INTERFACE") && push(@ndr, ParseInterface($_));
		($_->{TYPE} eq "IMPORT") && push(@ndr, $_);
	}

	return \@ndr;
}

sub GetNextLevel($$)
{
	my $e = shift;
	my $fl = shift;

	my $seen = 0;

	foreach my $l (@{$e->{LEVELS}}) {
		return $l if ($seen);
		($seen = 1) if ($l == $fl);
	}

	return undef;
}

sub GetPrevLevel($$)
{
	my ($e,$fl) = @_;
	my $prev = undef;

	foreach my $l (@{$e->{LEVELS}}) {
		(return $prev) if ($l == $fl);
		$prev = $l;
	}

	return undef;
}

sub ContainsString($)
{
	my ($e) = @_;

	foreach my $l (@{$e->{LEVELS}}) {
		return 1 if ($l->{TYPE} eq "ARRAY" and $l->{IS_ZERO_TERMINATED});
	}

	return 0;
}

sub ContainsDeferred($$)
{
	my ($e,$l) = @_;

	return 1 if ($l->{CONTAINS_DEFERRED});

	while ($l = GetNextLevel($e,$l))
	{
		return 1 if ($l->{IS_DEFERRED}); 
		return 1 if ($l->{CONTAINS_DEFERRED});
	} 
	
	return 0;
}

sub el_name($)
{
	my $e = shift;

	if ($e->{PARENT} && $e->{PARENT}->{NAME}) {
		return "$e->{PARENT}->{NAME}.$e->{NAME}";
	}

	if ($e->{PARENT} && $e->{PARENT}->{PARENT}->{NAME}) {
		return "$e->{PARENT}->{PARENT}->{NAME}.$e->{NAME}";
	}

	if ($e->{PARENT}) {
		return "$e->{PARENT}->{NAME}.$e->{NAME}";
	}

	return $e->{NAME};
}

###################################
# find a sibling var in a structure
sub find_sibling($$)
{
	my($e,$name) = @_;
	my($fn) = $e->{PARENT};

	if ($name =~ /\*(.*)/) {
		$name = $1;
	}

	for my $e2 (@{$fn->{ELEMENTS}}) {
		return $e2 if ($e2->{NAME} eq $name);
	}

	return undef;
}

my %property_list = (
	# interface
	"helpstring"		=> ["INTERFACE", "FUNCTION"],
	"version"		=> ["INTERFACE"],
	"uuid"			=> ["INTERFACE"],
	"endpoint"		=> ["INTERFACE"],
	"pointer_default"	=> ["INTERFACE"],
	"pointer_default_top"	=> ["INTERFACE"],
	"helper"		=> ["INTERFACE"],
	"authservice"		=> ["INTERFACE"],

	# dcom
	"object"		=> ["INTERFACE"],
	"local"			=> ["INTERFACE", "FUNCTION"],
	"iid_is"		=> ["ELEMENT"],
	"call_as"		=> ["FUNCTION"],
	"idempotent"		=> ["FUNCTION"],

	# function
	"noopnum"		=> ["FUNCTION"],
	"in"			=> ["ELEMENT"],
	"out"			=> ["ELEMENT"],
	"async"			=> ["FUNCTION"],

	# pointer
	"ref"			=> ["ELEMENT"],
	"ptr"			=> ["ELEMENT"],
	"unique"		=> ["ELEMENT"],
	"ignore"		=> ["ELEMENT"],
	"relative"		=> ["ELEMENT"],
	"relative_base"		=> ["TYPEDEF"],
	"null_is_ffffffff"	=> ["ELEMENT"],

	"gensize"		=> ["TYPEDEF"],
	"value"			=> ["ELEMENT"],
	"flag"			=> ["ELEMENT", "TYPEDEF"],

	# generic
	"public"		=> ["FUNCTION", "TYPEDEF"],
	"nopush"		=> ["FUNCTION", "TYPEDEF"],
	"nopull"		=> ["FUNCTION", "TYPEDEF"],
	"nosize"		=> ["FUNCTION", "TYPEDEF"],
	"noprint"		=> ["FUNCTION", "TYPEDEF"],
	"noejs"			=> ["FUNCTION", "TYPEDEF"],

	# union
	"switch_is"		=> ["ELEMENT"],
	"switch_type"		=> ["ELEMENT", "TYPEDEF"],
	"nodiscriminant"	=> ["TYPEDEF"],
	"case"			=> ["ELEMENT"],
	"default"		=> ["ELEMENT"],

	"represent_as"		=> ["ELEMENT"],
	"transmit_as"		=> ["ELEMENT"],

	# subcontext
	"subcontext"		=> ["ELEMENT"],
	"subcontext_size"	=> ["ELEMENT"],
	"compression"		=> ["ELEMENT"],

	# enum
	"enum8bit"		=> ["TYPEDEF"],
	"enum16bit"		=> ["TYPEDEF"],
	"v1_enum"		=> ["TYPEDEF"],

	# bitmap
	"bitmap8bit"		=> ["TYPEDEF"],
	"bitmap16bit"		=> ["TYPEDEF"],
	"bitmap32bit"		=> ["TYPEDEF"],
	"bitmap64bit"		=> ["TYPEDEF"],

	# array
	"range"			=> ["ELEMENT"],
	"size_is"		=> ["ELEMENT"],
	"string"		=> ["ELEMENT"],
	"noheader"		=> ["ELEMENT"],
	"charset"		=> ["ELEMENT"],
	"length_is"		=> ["ELEMENT"],
);

#####################################################################
# check for unknown properties
sub ValidProperties($$)
{
	my ($e,$t) = @_;

	return unless defined $e->{PROPERTIES};

	foreach my $key (keys %{$e->{PROPERTIES}}) {
		fatal($e, el_name($e) . ": unknown property '$key'\n")
			unless defined($property_list{$key});

   		fatal($e, el_name($e) . ": property '$key' not allowed on '$t'\n")
			unless grep($t, @{$property_list{$key}});
	}
}

sub mapToScalar($)
{
	my $t = shift;
	my $ti = getType($t);

	if (not defined ($ti)) {
		return undef;
	} elsif ($ti->{DATA}->{TYPE} eq "ENUM") {
		return Parse::Pidl::Typelist::enum_type_fn($ti->{DATA});
	} elsif ($ti->{DATA}->{TYPE} eq "BITMAP") {
		return Parse::Pidl::Typelist::enum_type_fn($ti->{DATA});
	} elsif ($ti->{DATA}->{TYPE} eq "SCALAR") {
		return $t;
	}

	return undef;
}

#####################################################################
# parse a struct
sub ValidElement($)
{
	my $e = shift;

	ValidProperties($e,"ELEMENT");

	# Check whether switches are used correctly.
	if (my $switch = has_property($e, "switch_is")) {
		my $e2 = find_sibling($e, $switch);
		my $type = getType($e->{TYPE});

		if (defined($type) and $type->{DATA}->{TYPE} ne "UNION") {
			fatal($e, el_name($e) . ": switch_is() used on non-union type $e->{TYPE} which is a $type->{DATA}->{TYPE}");
		}

		if (!has_property($type, "nodiscriminant") and defined($e2)) {
			my $discriminator_type = has_property($type, "switch_type");
			$discriminator_type = "uint32" unless defined ($discriminator_type);

			my $t1 = mapToScalar($discriminator_type);

			if (not defined($t1)) {
				fatal($e, el_name($e) . ": unable to map discriminator type '$discriminator_type' to scalar");
			}

			my $t2 = mapToScalar($e2->{TYPE});
			if (not defined($t2)) {
				fatal($e, el_name($e) . ": unable to map variable used for switch_is() to scalar");
			}

			if ($t1 ne $t2) {
				nonfatal($e, el_name($e) . ": switch_is() is of type $e2->{TYPE} ($t2), while discriminator type for union $type->{NAME} is $discriminator_type ($t1)");
			}
		}
	}


	if (has_property($e, "subcontext") and has_property($e, "represent_as")) {
		fatal($e, el_name($e) . " : subcontext() and represent_as() can not be used on the same element");
	}

	if (has_property($e, "subcontext") and has_property($e, "transmit_as")) {
		fatal($e, el_name($e) . " : subcontext() and transmit_as() can not be used on the same element");
	}

	if (has_property($e, "represent_as") and has_property($e, "transmit_as")) {
		fatal($e, el_name($e) . " : represent_as() and transmit_as() can not be used on the same element");
	}

	if (has_property($e, "represent_as") and has_property($e, "value")) {
		fatal($e, el_name($e) . " : represent_as() and value() can not be used on the same element");
	}

	if (defined (has_property($e, "subcontext_size")) and not defined(has_property($e, "subcontext"))) {
		fatal($e, el_name($e) . " : subcontext_size() on non-subcontext element");
	}

	if (defined (has_property($e, "compression")) and not defined(has_property($e, "subcontext"))) {
		fatal($e, el_name($e) . " : compression() on non-subcontext element");
	}

	if (!$e->{POINTERS} && (
		has_property($e, "ptr") or
		has_property($e, "unique") or
		has_property($e, "relative") or
		has_property($e, "ref"))) {
		my $dt = getType($e->{TYPE});
		fatal($e, el_name($e) . " : pointer properties on non-pointer element\n") unless ($dt->{DATA}{POINTERS});
	}
}

#####################################################################
# parse a struct
sub ValidStruct($)
{
	my($struct) = shift;

	ValidProperties($struct,"STRUCT");

	foreach my $e (@{$struct->{ELEMENTS}}) {
		$e->{PARENT} = $struct;
		ValidElement($e);
	}
}

#####################################################################
# parse a union
sub ValidUnion($)
{
	my($union) = shift;

	ValidProperties($union,"UNION");

	if (has_property($union->{PARENT}, "nodiscriminant") and has_property($union->{PARENT}, "switch_type")) {
		fatal($union->{PARENT}, $union->{PARENT}->{NAME} . ": switch_type() on union without discriminant");
	}
	
	foreach my $e (@{$union->{ELEMENTS}}) {
		$e->{PARENT} = $union;

		if (defined($e->{PROPERTIES}->{default}) and 
			defined($e->{PROPERTIES}->{case})) {
			fatal $e, "Union member $e->{NAME} can not have both default and case properties!\n";
		}
		
		unless (defined ($e->{PROPERTIES}->{default}) or 
				defined ($e->{PROPERTIES}->{case})) {
			fatal $e, "Union member $e->{NAME} must have default or case property\n";
		}

		if (has_property($e, "ref")) {
			fatal($e, el_name($e) . " : embedded ref pointers are not supported yet\n");
		}


		ValidElement($e);
	}
}

#####################################################################
# parse a typedef
sub ValidTypedef($)
{
	my($typedef) = shift;
	my $data = $typedef->{DATA};

	ValidProperties($typedef,"TYPEDEF");

	$data->{PARENT} = $typedef;

	if (ref($data) eq "HASH") {
		if ($data->{TYPE} eq "STRUCT") {
			ValidStruct($data);
		}

		if ($data->{TYPE} eq "UNION") {
			ValidUnion($data);
		}
	}
}

#####################################################################
# parse a function
sub ValidFunction($)
{
	my($fn) = shift;

	ValidProperties($fn,"FUNCTION");

	foreach my $e (@{$fn->{ELEMENTS}}) {
		$e->{PARENT} = $fn;
		if (has_property($e, "ref") && !$e->{POINTERS}) {
			fatal $e, "[ref] variables must be pointers ($fn->{NAME}/$e->{NAME})\n";
		}
		ValidElement($e);
	}
}

#####################################################################
# parse the interface definitions
sub ValidInterface($)
{
	my($interface) = shift;
	my($data) = $interface->{DATA};

	if (has_property($interface, "helper")) {
		nonfatal $interface, "helper() is pidl-specific and deprecated. Use `include' instead";
	}

	ValidProperties($interface,"INTERFACE");

	if (has_property($interface, "pointer_default")) {
		if (not grep (/$interface->{PROPERTIES}->{pointer_default}/, 
					("ref", "unique", "ptr"))) {
			fatal $interface, "Unknown default pointer type `$interface->{PROPERTIES}->{pointer_default}'";
		}
	}

	if (has_property($interface, "object")) {
     		if (has_property($interface, "version") && 
			$interface->{PROPERTIES}->{version} != 0) {
			fatal $interface, "Object interfaces must have version 0.0 ($interface->{NAME})\n";
		}

		if (!defined($interface->{BASE}) && 
			not ($interface->{NAME} eq "IUnknown")) {
			fatal $interface, "Object interfaces must all derive from IUnknown ($interface->{NAME})\n";
		}
	}
		
	foreach my $d (@{$data}) {
		($d->{TYPE} eq "TYPEDEF") &&
		    ValidTypedef($d);
		($d->{TYPE} eq "FUNCTION") && 
		    ValidFunction($d);
	}

}

#####################################################################
# Validate an IDL structure
sub Validate($)
{
	my($idl) = shift;

	foreach my $x (@{$idl}) {
		($x->{TYPE} eq "INTERFACE") && 
		    ValidInterface($x);
		($x->{TYPE} eq "IMPORTLIB") &&
			nonfatal($x, "importlib() not supported");
	}
}

1;
