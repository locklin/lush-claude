#!/usr/bin/env python3
"""Export a simple MNIST MLP as a TorchScript model.

Usage:
    python export_mnist_mlp.py [output_path]

Requires: torch
Output: mnist_mlp.pt (TorchScript format, loadable by torch9)

The model architecture:
    Input: [batch, 784] (flattened 28x28 images)
    fc1: Linear(784, 128) + ReLU
    fc2: Linear(128, 10)
    Output: [batch, 10] (logits)
"""

import sys
import torch
import torch.nn as nn


class MNISTMLP(nn.Module):
    def __init__(self):
        super().__init__()
        self.fc1 = nn.Linear(784, 128)
        self.fc2 = nn.Linear(128, 10)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = torch.relu(self.fc1(x))
        x = self.fc2(x)
        return x


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else "mnist_mlp.pt"

    print("Creating MNIST MLP model...")
    model = MNISTMLP()
    model.eval()

    print("Scripting model...")
    scripted = torch.jit.script(model)

    print(f"Saving to {output}...")
    scripted.save(output)
    print(f"Done. Model saved to {output}")
    print(f"Load in Lush: (torch9-model-load \"{output}\")")

if __name__ == "__main__":
    main()
