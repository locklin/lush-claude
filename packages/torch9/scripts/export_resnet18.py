#!/usr/bin/env python3
"""Export pretrained ResNet18 as a TorchScript model.

Usage:
    python export_resnet18.py [output_path]

Requires: torch, torchvision
Output: resnet18.pt (TorchScript format, loadable by torch9)
"""

import sys
import torch
import torchvision

def main():
    output = sys.argv[1] if len(sys.argv) > 1 else "resnet18.pt"

    print("Loading pretrained ResNet18...")
    model = torchvision.models.resnet18(weights="DEFAULT")
    model.eval()

    print("Tracing model with example input [1, 3, 224, 224]...")
    example_input = torch.randn(1, 3, 224, 224)
    traced = torch.jit.trace(model, example_input)

    print(f"Saving to {output}...")
    traced.save(output)
    print(f"Done. Model saved to {output}")
    print(f"Load in Lush: (torch9-model-load \"{output}\")")

if __name__ == "__main__":
    main()
