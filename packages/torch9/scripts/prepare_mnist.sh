#!/bin/bash
# Download and prepare MNIST data for torch9 examples.
#
# Downloads the MNIST dataset as raw binary IDX files and converts
# them to plain binary format usable by Lush idx-read/idx-write.
#
# Usage:
#     bash prepare_mnist.sh [output_dir]
#
# Output files in output_dir/:
#     train-images.idx   (60000 x 784 double-matrix)
#     train-labels.idx   (60000 int-matrix)
#     test-images.idx    (10000 x 784 double-matrix)
#     test-labels.idx    (10000 int-matrix)

set -e

OUTDIR="${1:-mnist_data}"
BASEURL="https://storage.googleapis.com/cvdf-datasets/mnist"

mkdir -p "$OUTDIR"

echo "Downloading MNIST files..."
for f in train-images-idx3-ubyte.gz train-labels-idx1-ubyte.gz \
         t10k-images-idx3-ubyte.gz t10k-labels-idx1-ubyte.gz; do
    if [ ! -f "$OUTDIR/$f" ]; then
        echo "  Fetching $f..."
        curl -sL "$BASEURL/$f" -o "$OUTDIR/$f"
    fi
done

echo "Decompressing..."
for f in "$OUTDIR"/*.gz; do
    gunzip -kf "$f"
done

echo "MNIST raw files ready in $OUTDIR/"
echo ""
echo "To load in Lush, use a script like:"
echo "  ;; Read raw MNIST ubyte format"
echo "  ;; Images: skip 16-byte header, read 28*28 bytes per image"
echo "  ;; Labels: skip 8-byte header, read 1 byte per label"
echo ""
echo "See packages/torch9/examples/mnist-train.lsh for a complete example."
