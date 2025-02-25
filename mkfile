</$objtype/mkfile

TARG=ttffs
OFILES=ttffs.$O 

HFILES=stb_truetype.h

BIN=/$objtype/bin
</sys/src/cmd/mkone

$O.pout: $OFILES
	$LD -o $O.pout -p $OFILES
